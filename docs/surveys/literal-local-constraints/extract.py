"""Find function-local variables initialized with a bare numeric literal in GDScript files.

Usage: python extract.py CORPUS_DIR

CORPUS_DIR holds the cloned repositories, one per subdirectory. Prints per-repository
statistics (all .gd files, and excluding addons/) and writes CORPUS_DIR/locals.json.
"""
import json
import os
import re
import sys

LIT = r'[+-]?(?:0x[0-9A-Fa-f_]+|0b[01_]+|(?:\d[\d_]*\.?[\d_]*|\.\d[\d_]*)(?:[eE][+-]?\d+)?)'
VAR_RE = re.compile(r'^(\s+)var\s+([A-Za-z_]\w*)\s*(:=|=)\s*(' + LIT + r')\s*(?:#.*)?$')
FUNC_RE = re.compile(r'^(\s*)(?:static\s+)?func\s+(\w+)')


def indent_width(s):
    w = 0
    for ch in s:
        if ch == '\t':
            w += 4
        elif ch == ' ':
            w += 1
        else:
            break
    return w


def is_float_lit(lit):
    lit = lit.lstrip('+-')
    if lit.startswith('0x') or lit.startswith('0b'):
        return False
    return '.' in lit or 'e' in lit.lower()


def scan(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    funcs = []
    for i, ln in enumerate(lines):
        m = FUNC_RE.match(ln)
        if not m:
            continue
        ind = indent_width(m.group(1))
        # Skip the rest of a multi-line signature.
        depth = ln.split('#')[0].count('(') - ln.split('#')[0].count(')')
        j = i + 1
        while depth > 0 and j < len(lines):
            t = lines[j].split('#')[0]
            depth += t.count('(') - t.count(')')
            j += 1
        # The body is every following line indented deeper than `func`.
        last = i
        while j < len(lines):
            s = lines[j]
            if s.strip() == '' or s.strip().startswith('#'):
                j += 1
                continue
            if indent_width(s) <= ind:
                break
            last = j
            j += 1
        funcs.append((i, last, m.group(2), ind))
    found = []
    for fs, fe, fname, find in funcs:
        for k in range(fs + 1, fe + 1):
            m = VAR_RE.match(lines[k])
            if m and indent_width(m.group(1)) > find:
                found.append(dict(line=k + 1, name=m.group(2), op=m.group(3), lit=m.group(4),
                                  kind='float' if is_float_lit(m.group(4)) else 'int',
                                  func=fname, fstart=fs + 1, fend=fe + 1))
    return found, len(lines)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    repos = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d, '.git')))
    out = []
    for repo in repos:
        stats = dict(files=0, lines=0, locals=0, files_noaddon=0, lines_noaddon=0, locals_noaddon=0)
        for dirpath, _, filenames in os.walk(os.path.join(root, repo)):
            if '.git' in dirpath.split(os.sep):
                continue
            for fn in filenames:
                if not fn.endswith('.gd'):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, os.path.join(root, repo)).replace('\\', '/')
                found, n = scan(path)
                addon = '/addons/' in '/' + rel
                stats['files'] += 1
                stats['lines'] += n
                stats['locals'] += len(found)
                if not addon:
                    stats['files_noaddon'] += 1
                    stats['lines_noaddon'] += n
                    stats['locals_noaddon'] += len(found)
                for item in found:
                    item.update(repo=repo, file=rel, addon=addon)
                    out.append(item)
        print(repo, stats)
    with open(os.path.join(root, 'locals.json'), 'w') as f:
        json.dump(out, f, indent=1)
    print('total', len(out))


main()
