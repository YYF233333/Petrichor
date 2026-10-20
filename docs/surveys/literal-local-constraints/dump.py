"""Print each candidate found by extract.py with the lines of its function that mention it.

Usage: python dump.py CORPUS_DIR [all|noaddon|addon]

The output is the input for manual classification of each variable's constraints.
"""
import json
import os
import re
import sys


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    sel = sys.argv[2] if len(sys.argv) == 3 else 'all'
    with open(os.path.join(root, 'locals.json')) as f:
        candidates = json.load(f)
    for idx, c in enumerate(candidates):
        if sel == 'noaddon' and c['addon']:
            continue
        if sel == 'addon' and not c['addon']:
            continue
        with open(os.path.join(root, c['repo'], c['file']), encoding='utf-8', errors='replace') as f:
            lines = f.read().split('\n')
        word = re.compile(r'(?<![\w.$])' + re.escape(c['name']) + r'(?!\w)')
        print(f"=== #{idx} {c['repo']}/{c['file']}:{c['line']}  func {c['func']} "
              f"({c['fstart']}-{c['fend']})  var {c['name']} {c['op']} {c['lit']}")
        print(f"  {c['fstart']}: {lines[c['fstart'] - 1].rstrip()}")
        for k in range(c['line'], c['fend'] + 1):
            if word.search(lines[k - 1].split('#')[0]):
                print(f"  {k}: {lines[k - 1].rstrip()}")


main()
