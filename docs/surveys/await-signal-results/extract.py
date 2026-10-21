"""Find every await expression in GDScript files, classify its operand, and check whether its result is used.

Usage: python extract.py CORPUS_DIR GODOT_DIR

CORPUS_DIR holds the cloned repositories, one per subdirectory. GODOT_DIR is a Godot
checkout; its class reference (doc/classes/*.xml and */doc_classes/*.xml) gives the
parameters of engine signals. Manual decisions recorded in classification.json, next to
this script, override the automatic classification. Prints the corpus table and the
result tables, and writes CORPUS_DIR/awaits.json for dump.py and CORPUS_DIR/connects.json
(the `.connect()` sites counted for context).
"""
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

# Vendored addons, written by authors other than the repository's own, are not first-party code
# (prefixes relative to the repository root). An addon that is the repository's own product, such as
# beehave/addons/beehave, is first-party. For Pixelorama, keychain (same organization) and applinks
# (written for Pixelorama) are first-party; the authorship of the others is not evident, so they count as vendored.
VENDORED = {
    'beehave': ['addons/gdUnit4/'],
    'godot-open-rpg': ['addons/dialogic/'],
    'godot-statecharts': ['addons/gut/'],
    'Pixelorama': ['addons/aimg_io/', 'addons/dockable_container/', 'addons/gdgifexporter/', 'addons/SmartSlicer/'],
}
# Test suites of first-party code.
TESTS = {
    'beehave': ['test/'],
    'gdUnit4': ['addons/gdUnit4/test/'],
    'godot_dialogue_manager': ['tests/'],
    'godot-statecharts': ['tests/'],
}

IDENT = re.compile(r'[A-Za-z_]\w*')
NODE_PATH = re.compile(r'[$%](?:"[^"\n]*"|\'[^\'\n]*\'|/?%?\w+(?:/%?\w+)*)')
AWAIT = re.compile(r'(?<![\w.])await\b')
CONT_CHARS = set('([{,=+-*/%<>!&|^~.')
OP_WORDS = {'and', 'or', 'not', 'return', 'in', 'is', 'as', 'if', 'elif', 'while', 'else', 'match', 'when'}


# ---------------------------------------------------------------- lexical helpers

def blank(src):
    """Replace comments and the contents of string literals with spaces, keeping newlines and offsets."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '#':
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c in '"\'':
            q = c * 3 if src.startswith(c * 3, i) else c
            j = i + len(q)
            while j < n:
                if src[j] == '\\' and j + 1 < n:
                    j += 2
                    continue
                if src.startswith(q, j) or (len(q) == 1 and src[j] == '\n'):
                    break
                j += 1
            for k in range(i + len(q), min(j, n)):
                if out[k] != '\n':
                    out[k] = ' '
            i = j + (len(q) if src.startswith(q, j) else 0)
        else:
            i += 1
    return ''.join(out)


def match_bracket(s, p):
    """Index of the bracket closing the one at p (strings and comments are already blanked)."""
    depth = 0
    for k in range(p, len(s)):
        if s[k] in '([{':
            depth += 1
        elif s[k] in ')]}':
            depth -= 1
            if depth == 0:
                return k
    return len(s) - 1


def skip_h(s, p):
    """Skip spaces, tabs and backslash line continuations."""
    n = len(s)
    while p < n:
        if s[p] in ' \t\r':
            p += 1
        elif s[p] == '\\' and p + 1 < n and s[p + 1] == '\n':
            p += 2
        else:
            break
    return p


def skip_all(s, p):
    while p < len(s) and s[p] in ' \t\r\n\\':
        p += 1
    return p


def parse_chain(s, p, in_brackets):
    """Parse a primary expression and its postfix chain (attribute, call, subscript) at p.

    This is the operand of await: in GDScript, await binds tighter than every operator
    except call, attribute and subscript (gdscript_parser.h, PREC_AWAIT).
    Returns (end, primary, ops) or None. primary is (kind, start, end, text); ops are
    ('attr', name), ('call', open, close) or ('index', open, close).
    """
    p = skip_h(s, p)
    n = len(s)
    if p >= n:
        return None
    c = s[p]
    if c == '(':
        q = match_bracket(s, p)
        prim = ('group', p, q + 1, s[p:q + 1])
    elif c in '$%':
        m = NODE_PATH.match(s, p)
        if not m:
            return None
        prim = ('node', p, m.end(), m.group())
    elif c in '[{':
        q = match_bracket(s, p)
        prim = ('literal', p, q + 1, s[p:q + 1])
    elif c in '"\'':
        q = s.find(c, p + 1)
        prim = ('literal', p, q + 1, s[p:q + 1])
    else:
        m = IDENT.match(s, p)
        if not m:
            return None
        prim = ('ident', p, m.end(), m.group())
    p = prim[2]
    ops = []
    while True:
        q = skip_h(s, p)
        if in_brackets and q < n and s[q] == '\n':
            r = skip_all(s, q)
            if r < n and s[r] == '.' and not s.startswith('..', r):
                q = r
        if q < n and s[q] == '.' and not s.startswith('..', q):
            m = IDENT.match(s, skip_h(s, q + 1))
            if not m:
                break
            ops.append(('attr', m.group()))
            p = m.end()
        elif q < n and s[q] == '(':
            e = match_bracket(s, q)
            ops.append(('call', q, e))
            p = e + 1
        elif q < n and s[q] == '[':
            e = match_bracket(s, q)
            ops.append(('index', q, e))
            p = e + 1
        else:
            break
    return p, prim, ops


def count_params(text):
    """Number of comma-separated parameters at the top level of text."""
    depth, parts, cur = 0, [], ''
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    return len([x for x in parts if x.strip()])


def indent_width(line):
    w = 0
    for ch in line:
        if ch == '\t':
            w += 4
        elif ch == ' ':
            w += 1
        else:
            break
    return w


# ---------------------------------------------------------------- engine class reference

def load_engine(godot):
    files = [os.path.join(godot, 'doc', 'classes', f) for f in os.listdir(os.path.join(godot, 'doc', 'classes'))]
    for top in ('modules', 'platform'):
        for dirpath, _, filenames in os.walk(os.path.join(godot, top)):
            if os.path.basename(dirpath) == 'doc_classes':
                files += [os.path.join(dirpath, f) for f in filenames]
    classes = {}
    for path in files:
        if not path.endswith('.xml'):
            continue
        root = ET.parse(path).getroot()
        if root.tag != 'class':
            continue
        cls = dict(inherits=root.get('inherits'), signals={}, methods={}, members={})
        for sig in root.iter('signal'):
            cls['signals'][sig.get('name')] = len(sig.findall('param'))
        for meth in root.iter('method'):
            ret = meth.find('return')
            cls['methods'][meth.get('name')] = ret.get('type') if ret is not None else 'void'
        for mem in root.iter('member'):
            cls['members'][mem.get('name')] = mem.get('type')
        classes[root.get('name')] = cls
    singletons = dict(classes['@GlobalScope']['members'])
    by_name = defaultdict(set)
    for cname, cls in classes.items():
        for sname, arity in cls['signals'].items():
            by_name[sname].add((cname, arity))
    return classes, singletons, by_name


# ---------------------------------------------------------------- per-file index

FUNC_RE = re.compile(r'^([ \t]*)(?:static[ \t]+)?func[ \t]+(\w+)[ \t]*\(', re.M)
SIGNAL_RE = re.compile(r'^[ \t]*(?:@\w+[ \t]+)*signal[ \t]+(\w+)', re.M)
CLASS_NAME_RE = re.compile(r'^(?:@\w+(?:\([^)\n]*\))?[ \t]+)*class_name[ \t]+(\w+)', re.M)
EXTENDS_RE = re.compile(r'^(?:@\w+(?:\([^)\n]*\))?[ \t]+)*(?:class_name[ \t]+\w+[ \t]+)?extends[ \t]+'
                        r'(?:"([^"\n]+)"|\'([^\'\n]+)\'|([\w.]+))', re.M)
TYPED_VAR_RE = re.compile(r'\bvar[ \t]+(\w+)[ \t]*:[ \t]*([A-Za-z_]\w*(?:\.\w+)?)')
NEW_VAR_RE = re.compile(r'\bvar[ \t]+(\w+)[ \t]*(?::[ \t]*)?=[ \t]*([A-Za-z_]\w*)\.new\(')
AS_VAR_RE = re.compile(r'\bvar[ \t]+(\w+)[ \t]*(?::[ \t]*)?=[^\n]*?[ \t]as[ \t]+([A-Za-z_]\w*)[ \t]*$', re.M)
INIT_VAR_RE = re.compile(r'\bvar[ \t]+(\w+)[ \t]*(?::[ \t]*)?=[ \t]*')
FOR_RE = re.compile(r'\bfor[ \t]+(\w+)[ \t]*:[ \t]*([A-Za-z_]\w*)')
PARAM_RE = re.compile(r'(\w+)[ \t]*:[ \t]*([A-Za-z_]\w*)')


class FileInfo:
    def __init__(self, repo, rel, src):
        self.repo, self.rel, self.src = repo, rel, src
        self.code = blank(src)
        self.nlines = len(src.split('\n'))
        top = [ln for ln in src.split('\n') if not ln.startswith((' ', '\t'))]
        top = '\n'.join(top)
        m = CLASS_NAME_RE.search(top)
        self.class_name = m.group(1) if m else None
        m = EXTENDS_RE.search(top)
        self.extends = (m.group(1) or m.group(2) or m.group(3)) if m else None
        self.extends_is_path = bool(m and (m.group(1) or m.group(2)))
        self.signals = {}
        for m in SIGNAL_RE.finditer(self.code):
            q = skip_all(self.code, m.end())
            if q < len(self.code) and self.code[q] == '(':
                e = match_bracket(self.code, q)
                self.signals[m.group(1)] = count_params(self.code[q + 1:e])
            else:
                self.signals[m.group(1)] = 0
        self.funcs = defaultdict(list)       # name -> [(has_await, return type)]
        self.func_spans = []                 # (start, end, name)
        lines = self.code.split('\n')
        offsets = [0]
        for ln in lines:
            offsets.append(offsets[-1] + len(ln) + 1)
        self.offsets = offsets
        self.types = defaultdict(set)        # identifier -> {type name | ('init', pos)}
        for m in FUNC_RE.finditer(self.code):
            ind = indent_width(m.group(1))
            po = m.end() - 1
            pc = match_bracket(self.code, po)
            for pm in PARAM_RE.finditer(self.code[po + 1:pc]):
                self.types[pm.group(1)].add(pm.group(2))
            colon = self.code.find(':', pc)
            head = self.code[pc + 1:colon]
            rm = re.search(r'->[ \t]*([\w.]+)', head)
            ret = rm.group(1) if rm else None
            line0 = self.line_of(colon)
            body_end = colon + 1
            rest = self.code[colon + 1:offsets[line0]].strip() if line0 < len(lines) else ''
            if rest:
                body_end = offsets[line0] - 1
            else:
                k = line0
                while k < len(lines):
                    s = lines[k]
                    if s.strip() == '':
                        k += 1
                        continue
                    if indent_width(s) <= ind:
                        break
                    body_end = offsets[k] + len(s)
                    k += 1
            body = self.code[colon + 1:body_end]
            self.funcs[m.group(2)].append((bool(AWAIT.search(body)), ret))
            self.func_spans.append((m.start(), body_end, m.group(2)))
        for rx in (TYPED_VAR_RE, NEW_VAR_RE, AS_VAR_RE, FOR_RE):
            for m in rx.finditer(self.code):
                self.types[m.group(1)].add(m.group(2))
        for m in INIT_VAR_RE.finditer(self.code):
            if not self.types.get(m.group(1)):
                self.types[m.group(1)].add(('init', m.end()))

    def line_of(self, pos):
        lo, hi = 0, len(self.offsets) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.offsets[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    def func_at(self, pos):
        best = None
        for s, e, name in self.func_spans:
            if s <= pos <= e and (best is None or s >= best[0]):
                best = (s, e, name)
        return best[2] if best else None


# ---------------------------------------------------------------- resolution

class Resolver:
    def __init__(self, files, engine):
        self.classes, self.singletons, self.engine_by_name = engine
        # Lookups by name are scoped to one Godot project: (repo, project root).
        self.class_names = defaultdict(list)     # (scope, class_name) -> [FileInfo]
        self.user_by_name = defaultdict(list)    # (scope, signal) -> [(FileInfo, arity)]
        self.funcs_by_name = defaultdict(list)   # (scope, func) -> [(FileInfo, has_await, ret)]
        self.by_rel = defaultdict(list)          # scope -> [FileInfo]
        for f in files:
            scope = (f.repo, f.project)
            self.by_rel[scope].append(f)
            if f.class_name:
                self.class_names[(scope, f.class_name)].append(f)
            for sname, arity in f.signals.items():
                self.user_by_name[(scope, sname)].append((f, arity))
            for fname, defs in f.funcs.items():
                for has_await, ret in defs:
                    self.funcs_by_name[(scope, fname)].append((f, has_await, ret))

    @staticmethod
    def closest(f, cands):
        def common(g):
            a, b = f.rel.split('/'), g.rel.split('/')
            k = 0
            while k < min(len(a), len(b)) and a[k] == b[k]:
                k += 1
            return k
        return max(cands, key=common) if cands else None

    def user_class(self, f, name):
        name = name.split('.')[0]
        return self.closest(f, self.class_names.get(((f.repo, f.project), name), []))

    def base_of(self, f):
        """The class f extends: a FileInfo, an engine class name, or None."""
        if not f.extends:
            return 'RefCounted'
        if f.extends_is_path:
            path = f.extends
            if path.startswith('res://'):
                target = (f.project + '/' if f.project else '') + path[6:]
            else:
                target = os.path.normpath(os.path.join(os.path.dirname(f.rel), path)).replace('\\', '/')
            cands = [g for g in self.by_rel[(f.repo, f.project)] if g.rel == target]
            return self.closest(f, cands)
        g = self.user_class(f, f.extends)
        if g is not None:
            return g
        return f.extends if f.extends in self.classes else None

    def signal_in(self, t, name, seen=None):
        """Look up a signal in type t (FileInfo or engine class name). Returns (arity, source, owner) or None."""
        seen = seen or set()
        while t is not None:
            if isinstance(t, FileInfo):
                if id(t) in seen:
                    return None
                seen.add(id(t))
                if name in t.signals:
                    return t.signals[name], 'user', t.class_name or t.rel
                t = self.base_of(t)
            else:
                cls = self.classes.get(t)
                if cls is None:
                    return None
                if name in cls['signals']:
                    return cls['signals'][name], 'engine', t
                t = cls['inherits']
        return None

    def member_type(self, t, name, seen=None):
        seen = seen or set()
        while t is not None:
            if isinstance(t, FileInfo):
                if id(t) in seen:
                    return None
                seen.add(id(t))
                r = self.ident_type(t, name, depth=3)
                if r is not None:
                    return r
                t = self.base_of(t)
            else:
                cls = self.classes.get(t)
                if cls is None:
                    return None
                if name in cls['members']:
                    return self.named_type(None, cls['members'][name])
                t = cls['inherits']
        return None

    def method_return(self, t, name, seen=None):
        seen = seen or set()
        while t is not None:
            if isinstance(t, FileInfo):
                if id(t) in seen:
                    return None
                seen.add(id(t))
                if name in t.funcs:
                    rets = {r for _, r in t.funcs[name]}
                    return self.named_type(t, rets.pop()) if len(rets) == 1 and None not in rets else None
                t = self.base_of(t)
            else:
                cls = self.classes.get(t)
                if cls is None:
                    return None
                if name in cls['methods']:
                    return self.named_type(None, cls['methods'][name])
                t = cls['inherits']
        return None

    def named_type(self, f, tname):
        if not tname:
            return None
        if tname in self.classes or tname in ('Signal',):
            return tname
        if f is not None:
            g = self.user_class(f, tname)
            if g is not None:
                return g
        return None

    def ident_type(self, f, name, depth):
        """Type of an identifier in file f from its annotations or initializers (scope-insensitive)."""
        types = f.types.get(name)
        if not types:
            return None
        resolved = set()
        for t in types:
            if isinstance(t, tuple):
                if depth <= 0:
                    return None
                r = parse_chain(f.code, t[1], False)
                if r is None:
                    return None
                rt = self.chain_type(f, r[1], r[2], depth - 1)
            else:
                rt = self.named_type(f, t)
            if rt is None:
                return None
            resolved.add(rt if isinstance(rt, str) else id(rt))
            last = rt
        return last if len(resolved) == 1 else None

    def chain_type(self, f, prim, ops, depth=3):
        """Static type of a primary + postfix chain, or None if unknown."""
        kind, _, _, text = prim
        if kind == 'node':
            t = 'Node'
        elif kind == 'ident':
            if text == 'self':
                t = f
            elif ops and ops[0][0] == 'call':
                t = self.method_return(f, text)
                if t is None and text in self.classes:
                    t = text     # constructor-like use is rare; keep the class
                ops = ops[1:]
            elif text in self.singletons:
                t = self.singletons[text]
            else:
                t = self.ident_type(f, text, depth)
                if t is None:
                    t = self.member_type(self.base_of(f), text)
                if t is None and (text in self.classes or self.user_class(f, text)):
                    t = ('static', text)
        else:
            return None
        i = 0
        while i < len(ops) and t is not None:
            op = ops[i]
            if op[0] == 'attr':
                if i + 1 < len(ops) and ops[i + 1][0] == 'call':
                    if isinstance(t, tuple):
                        t = self.method_return(self.named_type(f, t[1]), op[1])
                    else:
                        t = self.method_return(t, op[1])
                    i += 2
                    continue
                if isinstance(t, tuple):
                    return None
                t = self.member_type(t, op[1])
            else:
                return None
            i += 1
        return None if isinstance(t, tuple) else t

    def signal_by_name(self, f, name):
        """Arity of a signal from its name alone: unique among engine signals and this project's user signals."""
        cands = set()
        for cname, arity in self.engine_by_name.get(name, ()):
            cands.add(('engine', arity))
        for g, arity in self.user_by_name.get(((f.repo, f.project), name), ()):
            cands.add(('user', arity))
        arities = {a for _, a in cands}
        sources = {s for s, _ in cands}
        if len(arities) == 1:
            return arities.pop(), '/'.join(sorted(sources))
        return (None, 'ambiguous') if cands else None

    def func_in(self, t, name, seen=None):
        seen = seen or set()
        while isinstance(t, FileInfo):
            if id(t) in seen:
                return None
            seen.add(id(t))
            if name in t.funcs:
                return t.funcs[name]
            t = self.base_of(t)
        if isinstance(t, str) and t in self.classes:
            c = t
            while c:
                if name in self.classes[c]['methods']:
                    return 'engine'
                c = self.classes[c]['inherits']
        return None

    def classify_call(self, f, prim, ops):
        callee = None
        if len(ops) >= 2 and ops[-2][0] == 'attr':
            callee = ops[-2][1]
            recv = self.chain_type(f, prim, ops[:-2])
        elif len(ops) == 1 and prim[0] == 'ident':
            callee = prim[3]
            recv = f
        else:
            return dict(kind='call', callee=None, detail='unresolved')
        if callee == 'Signal' and recv is f:
            return dict(kind='signal', source='Signal()', arity=None, signal='Signal(...)')
        defs = self.func_in(recv, callee) if recv is not None else None
        if defs == 'engine':
            return dict(kind='call', callee=callee, detail='engine method')
        how = 'receiver type'
        if defs is None:
            defs = [(a, r) for g, a, r in self.funcs_by_name.get(((f.repo, f.project), callee), ())]
            how = 'by name'
        if not defs:
            return dict(kind='call', callee=callee, detail='unresolved')
        if any(a for a, _ in defs):
            return dict(kind='coroutine', callee=callee, detail=how)
        if all(r == 'Signal' for _, r in defs):
            return dict(kind='signal', source='returned Signal', arity=None, signal=callee + '()')
        return dict(kind='call', callee=callee, detail='callee has no await (' + how + ')')

    def classify_operand(self, f, prim, ops):
        if prim[0] == 'group' and not ops:
            r = parse_chain(f.code, prim[1] + 1, True)
            if r and skip_all(f.code, r[0]) == prim[2] - 1:
                return self.classify_operand(f, r[1], r[2])
            return dict(kind='other', detail='expression')
        if prim[0] == 'ident' and prim[3] == 'func':
            return dict(kind='other', detail='lambda')
        if ops and ops[-1][0] == 'call':
            return self.classify_call(f, prim, ops)
        if ops and ops[-1][0] == 'index':
            return dict(kind='other', detail='subscript')
        if not ops and prim[0] != 'ident':
            return dict(kind='other', detail=prim[0])
        if ops:
            name = ops[-1][1]
            recv = self.chain_type(f, prim, ops[:-1])
            recv_text = 'typed' if recv is not None else 'untyped'
        else:
            name = prim[3]
            recv = f
            recv_text = 'self'
            t = self.ident_type(f, name, 2)
            if t == 'Signal':
                return dict(kind='signal', source='Signal value', arity=None, signal=name)
        if recv is not None and recv != 'Signal':
            found = self.signal_in(recv, name)
            if found:
                arity, source, owner = found
                return dict(kind='signal', source=source, arity=arity, signal=f'{owner}.{name}',
                            how='receiver ' + recv_text)
        if ops:
            t = self.chain_type(f, prim, ops)
            if t == 'Signal':
                return dict(kind='signal', source='Signal value', arity=None, signal=name)
        byname = self.signal_by_name(f, name)
        if byname:
            arity, source = byname
            return dict(kind='signal', source=source if arity is not None else 'ambiguous', arity=arity,
                        signal=name, how='by name')
        return dict(kind='other', detail='no signal named ' + name)


# ---------------------------------------------------------------- use of the result

def prev_significant(s, p):
    """Index of the last significant character before p, and whether a real newline lies between."""
    i, crossed = p - 1, False
    while i >= 0:
        c = s[i]
        if c in ' \t\r':
            i -= 1
        elif c == '\n':
            j = i - 1
            while j >= 0 and s[j] in ' \t\r':
                j -= 1
            if j >= 0 and s[j] == '\\':
                i = j - 1
            else:
                crossed = True
                i -= 1
        else:
            break
    return i, crossed


def word_before(s, i):
    j = i
    while j >= 0 and (s[j].isalnum() or s[j] == '_'):
        j -= 1
    return s[j + 1:i + 1]


def classify_use(s, pos, end, stack):
    """Decide whether the await at pos (operand ending at end) is a statement, and if not, how its value is used."""
    i, crossed = prev_significant(s, pos)
    c = s[i] if i >= 0 else None
    word = word_before(s, i) if c and (c.isalnum() or c == '_') else ''
    depth = len(stack)
    lambda_colon = False
    if c is None or c == ';':
        start = True
    elif c == ':':
        start = True
        if stack and stack[-1][0] == '{' and 'func' not in s[stack[-1][1]:i]:
            start = False
        lambda_colon = depth > 0
    elif crossed:
        start = depth == 0 or (c not in CONT_CHARS and word not in OP_WORDS)
    else:
        start = False
    if start:
        q = skip_h(s, end)
        if q >= len(s) or s[q] in '\n;':
            return 'statement'
        if s[q] in ')]},' and lambda_colon:
            return 'statement'
        return 'expression'
    if c == '=':
        p2 = s[i - 1] if i > 0 else ''
        if p2 in '=!<>':
            return 'comparison'
        if p2 in '+-*/%&|^':
            return 'compound assignment'
        k = s.rfind('\n', 0, i) + 1
        return 'var initializer' if re.match(r'[ \t]*(?:@\w+[ \t]+)*(?:static[ \t]+)?var\b', s[k:i]) else 'assignment'
    if word == 'return':
        return 'return'
    if word in ('if', 'elif', 'while', 'not', 'and', 'or', 'match', 'when', 'in'):
        return 'condition (' + word + ')'
    if c in '(,[{':
        if not stack:
            return 'other'
        b, bp = stack[-1]
        if b == '(':
            k = bp - 1
            while k >= 0 and s[k] in ' \t':
                k -= 1
            if k >= 0 and (s[k].isalnum() or s[k] in '_)]'):
                return 'argument'
            return 'grouped'
        return 'array element' if b == '[' else 'dictionary value'
    if c in '+-*/%<>&|^~!':
        return 'operand'
    return 'other'


# ---------------------------------------------------------------- main

def scan_awaits(f, resolver):
    s = f.code
    found = []
    positions = [m.start() for m in AWAIT.finditer(s)]
    if not positions:
        return found
    stack, k = [], 0
    for p in range(len(s)):
        while k < len(positions) and positions[k] == p:
            pos = positions[k]
            k += 1
            r = parse_chain(s, pos + len('await'), bool(stack))
            if r is None:
                found.append(dict(pos=pos, end=pos + 5, operand='', op=dict(kind='other', detail='unparsed'),
                                  use='unknown'))
                continue
            end, prim, ops = r
            op = resolver.classify_operand(f, prim, ops)
            use = classify_use(s, pos, end, list(stack))
            found.append(dict(pos=pos, end=end, operand=' '.join(f.src[prim[1]:end].split()), op=op, use=use))
        ch = s[p]
        if ch in '([{':
            stack.append((ch, p))
        elif ch in ')]}' and stack:
            stack.pop()
    return found


CONNECT_RE = re.compile(r'\.[ \t]*connect[ \t]*\(')


def chain_start(s, p):
    """Start of the postfix chain that ends just before p."""
    i = p - 1
    while i >= 0:
        c = s[i]
        if c in ')]':
            depth = 0
            while i >= 0:
                if s[i] in ')]':
                    depth += 1
                elif s[i] in '([':
                    depth -= 1
                    if depth == 0:
                        break
                i -= 1
            i -= 1
        elif c.isalnum() or c in '_.$%/':
            i -= 1
        else:
            break
    return i + 1


def scan_connects(f, resolver):
    """Signals used as `<signal>.connect(...)`, for context: how often multi-parameter signals are connected."""
    s = f.code
    out = []
    for m in CONNECT_RE.finditer(s):
        start = chain_start(s, m.start())
        if start == m.start():
            continue
        if s[skip_all(s, m.end())] in '"\'&':
            continue  # Object.connect("name", ...): the signal is named by a string
        r = parse_chain(s, start, True)
        if r is None:
            continue
        _, prim, ops = r
        k = next((i for i in range(len(ops) - 1) if ops[i] == ('attr', 'connect') and ops[i + 1][0] == 'call'
                  and ops[i + 1][1] == m.end() - 1), None)
        if k is None:
            continue
        op = resolver.classify_operand(f, prim, ops[:k])
        if op['kind'] == 'signal':
            out.append(dict(op, key=f'{f.repo}/{f.rel}:{f.line_of(m.start())}'))
    return out


def project_roots(path):
    """Directories (relative to the repository) that contain a project.godot. Several Godot projects can
    share a repository (godot-demo-projects has one per demo); class names and res:// paths are per project."""
    roots = set()
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = [d for d in dirnames if d != '.git']
        if 'project.godot' in filenames:
            rel = os.path.relpath(dirpath, path).replace('\\', '/')
            roots.add('' if rel == '.' else rel)
    if not roots:  # sparse checkouts may lack project.godot; the git tree still lists it
        try:
            out = subprocess.run(['git', '-C', path, 'ls-tree', '-r', '--name-only', 'HEAD'], capture_output=True,
                                 text=True, check=True).stdout.split('\n')
            roots = {os.path.dirname(p) for p in out if p == 'project.godot' or p.endswith('/project.godot')}
        except (OSError, subprocess.CalledProcessError):
            pass
    return sorted(roots, key=len, reverse=True)


def commit_of(path):
    try:
        return subprocess.run(['git', '-C', path, 'rev-parse', '--short=7', 'HEAD'], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return '?'


def category(repo, rel):
    if any(rel.startswith(x) for x in VENDORED.get(repo, ())):
        return 'vendored'
    if any(rel.startswith(x) for x in TESTS.get(repo, ())):
        return 'test'
    return 'first-party'


def load_classification():
    path = os.path.join(HERE, 'classification.json')
    if not os.path.exists(path):
        return {}
    with open(path, encoding='utf-8') as fh:
        data = json.load(fh)
    return {k: v for k, v in data.items() if not k.startswith('_')}


def arity_bucket(a):
    if a is None:
        return 'unknown'
    return '2+' if a >= 2 else str(a)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    root, godot = sys.argv[1], sys.argv[2]
    engine = load_engine(godot)
    repos = sorted((d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d, '.git'))), key=str.lower)
    files = []
    for repo in repos:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, repo)):
            dirnames[:] = sorted(d for d in dirnames if d != '.git')
            for fn in sorted(filenames):
                if fn.endswith('.gd'):
                    path = os.path.join(dirpath, fn)
                    rel = os.path.relpath(path, os.path.join(root, repo)).replace('\\', '/')
                    with open(path, encoding='utf-8', errors='replace') as fh:
                        files.append(FileInfo(repo, rel, fh.read()))
    roots = {repo: project_roots(os.path.join(root, repo)) for repo in repos}
    for f in files:
        f.project = next((r for r in roots[f.repo] if r == '' or f.rel.startswith(r + '/')), '')
    resolver = Resolver(files, engine)
    manual = load_classification()
    used_keys = set()
    records = []
    stats = {r: Counter() for r in repos}
    context = dict(connects=[], declared=[])
    for f in files:
        cat = category(f.repo, f.rel)
        st = stats[f.repo]
        st['files'] += 1
        st['lines'] += f.nlines
        st['lines ' + cat] += f.nlines
        st['raw awaits'] += len(AWAIT.findall(f.src))
        if cat != 'vendored':
            context['connects'] += [dict(op, category=cat) for op in scan_connects(f, resolver)]
            context['declared'] += [dict(arity=a, category=cat) for a in f.signals.values()]
        per_line = Counter()
        for a in scan_awaits(f, resolver):
            line = f.line_of(a['pos'])
            per_line[line] += 1
            key = f'{f.repo}/{f.rel}:{line}' + (f'#{per_line[line]}' if per_line[line] > 1 else '')
            rec = dict(key=key, repo=f.repo, file=f.rel, line=line, category=cat, func=f.func_at(a['pos']),
                       operand=a['operand'], use=a['use'], **a['op'])
            rec['auto'] = dict(a['op'], use=a['use'])
            if key in manual:
                rec.update(manual[key])
                rec['manual'] = True
                used_keys.add(key)
            rec['used'] = rec['use'] != 'statement'
            records.append(rec)
            st['awaits'] += 1
            st['awaits ' + cat] += 1
    for k in sorted(set(manual) - used_keys):
        print('warning: classification.json entry matches no await:', k, file=sys.stderr)
    with open(os.path.join(root, 'awaits.json'), 'w', encoding='utf-8') as fh:
        json.dump(records, fh, indent=1)
    with open(os.path.join(root, 'connects.json'), 'w', encoding='utf-8') as fh:
        json.dump(context['connects'], fh, indent=1)
    context['engine'] = [a for cls in engine[0].values() for a in cls['signals'].values()]
    commits = {r: commit_of(os.path.join(root, r)) for r in repos}
    report(repos, commits, stats, records, context)


def table(rows, header):
    print('| ' + ' | '.join(header) + ' |')
    print('|' + '---|' * len(header))
    for r in rows:
        print('| ' + ' | '.join(str(x) for x in r) + ' |')
    print()


KINDS = ['signal, engine class', 'signal, user-declared', 'signal, by name only', 'Signal value returned by a call',
         'signal declared outside the corpus', 'coroutine call', 'other call', 'other']


def kind_of(x):
    if x['kind'] == 'signal':
        src = x.get('source')
        if src in ('C#', 'plugin'):
            return 'signal declared outside the corpus'
        if src in ('returned Signal', 'Signal value', 'Signal()'):
            return 'Signal value returned by a call'
        if src == 'engine':
            return 'signal, engine class'
        if src == 'user':
            return 'signal, user-declared'
        return 'signal, by name only'
    return {'coroutine': 'coroutine call', 'call': 'other call'}.get(x['kind'], 'other')


def report(repos, commits, stats, records, context):
    tot = Counter()
    for r in repos:
        for k, v in stats[r].items():
            tot[k] += v
    fp_lines = tot['lines first-party'] + tot['lines test']
    scopes = [('all', lambda x: True, tot['lines']),
              ('first-party', lambda x: x['category'] != 'vendored', fp_lines),
              ('first-party, no tests', lambda x: x['category'] == 'first-party', tot['lines first-party'])]

    print('## Corpus\n')
    rows = []
    for r in repos + ['Total']:
        st = tot if r == 'Total' else stats[r]
        fp = st['lines first-party'] + st['lines test']
        rows.append([r, commits.get(r, ''), f"{st['files']:,}", f"{st['lines']:,}", f"{fp:,}",
                     f"{st['lines test']:,}", f"{st['awaits']:,}",
                     f"{st['awaits first-party'] + st['awaits test']:,}", f"{st['awaits test']:,}"])
    table(rows, ['repo', 'commit', '.gd files', 'lines', 'first-party lines', 'of which tests', 'awaits',
                 'first-party awaits', 'of which tests'])
    print(f"`await` tokens in source: {tot['raw awaits']:,}; in code (not comments or strings): {tot['awaits']:,}\n")

    print('## Awaits per 1,000 lines\n')
    table([[name, sum(1 for x in records if sel(x)), f'{n:,}', f"{1000 * sum(1 for x in records if sel(x)) / n:.2f}"]
           for name, sel, n in scopes], ['scope', 'awaits', 'lines', 'per 1,000 lines'])

    print('## Operand kinds\n')
    table([[k] + [sum(1 for x in records if sel(x) and kind_of(x) == k) for _, sel, _ in scopes] for k in KINDS]
          + [['total'] + [sum(1 for x in records if sel(x)) for _, sel, _ in scopes]],
          ['operand'] + [s[0] for s in scopes])

    print('## Signal awaits by arity and use\n')
    rows = []
    for b in ('0', '1', '2+', 'unknown', 'total'):
        row = [b]
        for _, sel, _ in scopes:
            xs = [x for x in records if sel(x) and x['kind'] == 'signal'
                  and (b == 'total' or arity_bucket(x.get('arity')) == b)]
            row.append(f"{sum(not x['used'] for x in xs)} / {sum(x['used'] for x in xs)}")
        rows.append(row)
    table(rows, ['arity', *[f'{s[0]}: statement / used' for s in scopes]])

    print('## How results are used (all scopes)\n')
    for b in ('0', '1', '2+', 'unknown'):
        c = Counter(x['use'] for x in records if x['kind'] == 'signal' and x['used']
                    and arity_bucket(x.get('arity')) == b)
        print(f'- signal, arity {b}: ' + ', '.join(f'{k} {v}' for k, v in c.most_common()))
    c = Counter(x['use'] for x in records if x['kind'] == 'coroutine' and x['used'])
    n = sum(1 for x in records if x['kind'] == 'coroutine')
    print(f'- coroutine calls ({sum(c.values())} of {n} used): ' + ', '.join(f'{k} {v}' for k, v in c.most_common()))
    print()

    print('## Awaits of signals with 2+ parameters\n')
    rows = [[x['key'], x['category'], x.get('signal'), x.get('arity'), x['use'], x.get('note', '')]
            for x in records if x['kind'] == 'signal' and (x.get('arity') or 0) >= 2]
    table(rows, ['site', 'category', 'signal', 'arity', 'use', 'note'])

    print('## Signal awaits of unknown arity\n')
    rows = [[x['key'], x['category'], x.get('signal'), x['use'], x.get('note', '')]
            for x in records if x['kind'] == 'signal' and x.get('arity') is None]
    table(rows, ['site', 'category', 'signal', 'use', 'note'])

    print('## Context: signal arity at connect sites and declarations (first-party)\n')
    rows = []
    for b in ('0', '1', '2+', 'unknown'):
        rows.append([b] + [sum(1 for x in context['connects'] if arity_bucket(x.get('arity')) == b
                               and (t or x['category'] != 'test')) for t in (True, False)]
                    + [sum(1 for x in context['declared'] if arity_bucket(x['arity']) == b)]
                    + [sum(1 for a in context['engine'] if arity_bucket(a) == b)])
    table(rows, ['arity', '`.connect()` sites', 'of which outside tests', 'GDScript signal declarations',
                 'engine signals (class reference)'])


main()
