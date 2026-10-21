"""Print each property found by extract.py with its declaration, setter and getter.

Usage: python dump.py CORPUS_DIR [first-party|test|vendored|duplicate|all] [getters|setters]

The output is the input for the manual classification recorded in classification.json.
Each property is followed by the mechanical evidence that extract.py collected: whether
the accessor mentions the property's own (backing) field, and which child nodes, @onready
members, members assigned from nodes in _ready, or tree accessors the accessor reaches
directly or through same-class calls, together with any guards it found on the way.
"""
import json
import os
import sys


def show(acc, label):
    if not acc:
        return
    flags = []
    if 'uses_field' in acc:
        flags.append('uses-field' if acc['uses_field'] else 'NO-FIELD')
    for k in ('trivial', 'writes_field', 'plain'):
        if acc.get(k):
            flags.append(k)
    name = f" = {acc['func']}" if acc['kind'] == 'named' else ''
    print(f"  [{label}{name}] {' '.join(flags)}")
    for ln, t in acc['lines'] or []:
        print(f"    {ln:5}| {t}")
    if acc.get('touches'):
        print('    touches: ' + '; '.join(acc['touches']))
    if acc.get('guards'):
        print('    guards:  ' + '; '.join(acc['guards']))


def main():
    if len(sys.argv) not in (2, 3, 4):
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    sel = sys.argv[2] if len(sys.argv) >= 3 else 'first-party'
    only = sys.argv[3] if len(sys.argv) == 4 else None
    with open(os.path.join(root, 'properties.json'), encoding='utf-8') as f:
        props = json.load(f)
    for idx, p in enumerate(props):
        if sel != 'all' and p['scope'] != sel:
            continue
        if only == 'getters' and not p['getter']:
            continue
        if only == 'setters' and (p['getter'] or not p['setter']):
            continue
        attrs = [p['form']]
        if p['static']:
            attrs.append('static')
        if p['export']:
            attrs.append('@' + ','.join(p['export_annots']))
        if p['onready']:
            attrs.append('@onready')
        attrs.append(f"type={p['type']}")
        attrs.append('init' if p['init'] is not None else 'no-init')
        print(f"=== #{idx} {p['id']}  {p['name']}  {' '.join(attrs)}")
        for ln, t in p['decl']:
            print(f"    {ln:5}| {t}")
        show(p['setter'], 'set')
        show(p['getter'], 'get')


main()
