"""Print awaits found by extract.py with their surrounding lines, for manual classification.

Usage: python dump.py CORPUS_DIR [SELECTION]

SELECTION is one of:
  all            every await (default)
  signals        every await whose operand is a signal
  used           every await whose result is used
  used-signals   signal awaits whose result is used
  multi          signal awaits with 2 or more parameters
  unknown        signal awaits of unknown arity
  byname         signal awaits whose arity was found from the signal's name alone
  nonsignal      awaits whose operand is not a signal or a coroutine call
  manual         awaits with an entry in classification.json
For a result that is assigned to a variable, the later lines of the function that mention
the variable are printed too.
"""
import json
import os
import re
import sys

SELECTIONS = {
    'all': lambda x: True,
    'signals': lambda x: x['kind'] == 'signal',
    'used': lambda x: x['used'],
    'used-signals': lambda x: x['kind'] == 'signal' and x['used'],
    'multi': lambda x: x['kind'] == 'signal' and (x.get('arity') or 0) >= 2,
    'unknown': lambda x: x['kind'] == 'signal' and x.get('arity') is None,
    'byname': lambda x: x['kind'] == 'signal' and x.get('how') == 'by name',
    'nonsignal': lambda x: x['kind'] not in ('signal', 'coroutine'),
    'manual': lambda x: x.get('manual'),
}


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


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in SELECTIONS):
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    sel = SELECTIONS[sys.argv[2] if len(sys.argv) == 3 else 'all']
    with open(os.path.join(root, 'awaits.json'), encoding='utf-8') as fh:
        records = json.load(fh)
    cache = {}
    for x in records:
        if not sel(x):
            continue
        path = os.path.join(root, x['repo'], x['file'])
        if path not in cache:
            with open(path, encoding='utf-8', errors='replace') as fh:
                cache[path] = fh.read().split('\n')
        lines = cache[path]
        i = x['line'] - 1
        detail = x.get('signal') or x.get('callee') or x.get('detail') or ''
        print(f"=== {x['key']}  [{x['category']}] func {x['func']}  kind={x['kind']} {x.get('source', '')} "
              f"{detail} arity={x.get('arity')}  use={x['use']}")
        print(f"    operand: {x['operand']}")
        if x.get('note'):
            print(f"    note: {x['note']}")
        for k in range(max(0, i - 2), min(len(lines), i + 3)):
            print(f"  {k + 1:5}{'>' if k == i else ' '} {lines[k].rstrip()}")
        m = re.match(r'\s*(?:var\s+)?(\w+)\s*(?::\s*[\w\[\], .]+)?\s*:?=\s*(?:\(\s*)?await\b', lines[i])
        if m and x['used']:
            name = m.group(1)
            word = re.compile(r'(?<![\w.$])' + re.escape(name) + r'(?!\w)')
            base = indent_width(lines[i])
            for k in range(i + 3, len(lines)):
                s = lines[k]
                if s.strip() and indent_width(s) < base and re.match(r'\s*(static\s+)?func\b', s):
                    break
                if s.strip() and indent_width(s) == 0:
                    break
                if word.search(s.split('#')[0]):
                    print(f"  {k + 1:5}  {s.rstrip()}")
        print()


main()
