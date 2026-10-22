"""Build catalog.tsv and catalog.md from raw/*.tsv and merges.py.

Usage: python build.py [OUT_DIR]

raw/ and merges.py are read from this script's directory. The two output
files are written to OUT_DIR, which defaults to this script's directory.
Python 3 standard library only.
"""
import csv, re, os, sys, collections, html
HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
from merges import MERGED, ORDER_ENGINE, ORDER_CS

RAW = os.path.join(HERE, 'raw')
BASE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else HERE
os.makedirs(BASE, exist_ok=True)
FIELDS = ['id', 'group', 'title', 'description', 'example', 'root_layer',
          'evidence_issues', 'evidence_docs', 'evidence_source', 'frequency',
          'status', 'fix_class', 'fix_reason']

raw = {}
for name in ['CS-A', 'CS-B', 'EG-A', 'EG-B', 'EG-C']:
    with open(os.path.join(RAW, name + '.tsv'), encoding='utf-8') as f:
        rows = list(csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE))
    assert rows[0] == FIELDS, (name, rows[0])
    for r in rows[1:]:
        if not r or not r[0].strip():
            continue
        assert len(r) == len(FIELDS), (name, r[0], len(r))
        raw[r[0]] = dict(zip(FIELDS, r))

used = set()
for m in MERGED.values():
    for s in m['source_rows'].split(', '):
        assert s in raw, s
        assert s not in used, s
        used.add(s)
for k in ORDER_ENGINE + ORDER_CS:
    if not k.startswith('M'):
        assert k not in used, k
        used.add(k)
missing = set(raw) - used
assert not missing, missing


def primary(fc):
    fc = fc.strip()
    if fc.startswith('N/A'):
        return 'N/A'
    m = re.match(r'([LTWME])', fc)
    return m.group(1)


def analog(fc):
    m = re.search(r'analog:\s*([LTWME])', fc)
    return m.group(1) if m else ''


def has_e(fc):
    # An E residual: the class string mentions E as a class letter anywhere.
    return bool(re.search(r'(^|[^A-Za-z])E([^A-Za-z]|$)', fc))


out = []
for prefix, order in (('G', ORDER_ENGINE), ('C', ORDER_CS)):
    for i, k in enumerate(order, 1):
        if k.startswith('M'):
            row = dict(MERGED[k])
            src = row.pop('source_rows')
        else:
            row = dict(raw[k])
            src = k
        row['id'] = '%s-%02d' % (prefix, i)
        row['source_rows'] = src
        row['primary_class'] = primary(row['fix_class'])
        for f in FIELDS:
            v = row[f]
            assert '\t' not in v and '\n' not in v, (row['id'], f)
        out.append(row)

OUT_FIELDS = ['id', 'group', 'title', 'description', 'example', 'root_layer',
              'evidence_issues', 'evidence_docs', 'evidence_source', 'frequency',
              'status', 'fix_class', 'primary_class', 'fix_reason', 'source_rows']

with open(os.path.join(BASE, 'catalog.tsv'), 'w', encoding='utf-8', newline='') as f:
    f.write('\t'.join(OUT_FIELDS) + '\n')
    for r in out:
        f.write('\t'.join(r[k] for k in OUT_FIELDS) + '\n')

# Counts
classes = ['L', 'T', 'W', 'M', 'E', 'N/A']
groups = ['engine/GDScript', 'C#']
cnt = collections.Counter((r['group'], r['primary_class']) for r in out)
ecnt = collections.Counter(r['group'] for r in out if has_e(r['fix_class']))
acnt = collections.Counter(analog(r['fix_class']) for r in out if r['primary_class'] == 'N/A')
layer = collections.Counter((r['group'], r['root_layer'].split(' (')[0]) for r in out)

lines = []
lines.append('| Group | ' + ' | '.join(classes) + ' | Total | Any E residual |')
lines.append('|---|' + '---|' * (len(classes) + 2))
for g in groups:
    tot = sum(cnt[(g, c)] for c in classes)
    lines.append('| %s | ' % g + ' | '.join(str(cnt[(g, c)]) for c in classes) + ' | %d | %d |' % (tot, ecnt[g]))
tot_all = [sum(cnt[(g, c)] for g in groups) for c in classes]
lines.append('| All | ' + ' | '.join(map(str, tot_all)) + ' | %d | %d |' % (len(out), sum(ecnt.values())))
matrix = '\n'.join(lines)

print(matrix)
print('N/A analogs:', dict(acnt))
print('layers:', dict(layer))
print('E residual ids:', [r['id'] + ' ' + r['fix_class'] for r in out if has_e(r['fix_class'])])


def md(s):
    s = html.escape(s, quote=False)
    s = s.replace(' | ', '<br>')
    s = s.replace('|', '\\|')
    return s


md_lines = []
md_lines.append('# Petrichor pitfall catalog')
md_lines.append('')
md_lines.append('Engine and GDScript pitfalls, and C# (.NET) bridge pitfalls, that bite Godot 4 script authors. Engine facts were checked by reading godotengine/godot at commit 469659e0617345149cd1eaec2e643d61d130aa6a (master, 4.8-beta); `path:line` refers to that commit. Issue states and reaction counts were read from GitHub on 2026-10-23. Nothing was confirmed by running the engine; claims that would need a run are marked "unverified (needs repro)".')
md_lines.append('')
md_lines.append('Fix classes (an inference for a new, non-.NET, statically typed script language and its toolchain): **L** language semantics; **T** static checking or toolchain; **W** runtime wrapper over public ClassDB/GDExtension APIs; **M** needs C++ APIs not exposed to GDExtension (module only); **E** needs an engine core change; **N/A** specific to .NET (the analog the new language would face is given in parentheses). A class such as "L+W (order itself: E)" means L and W remove the pitfall for code written in the new language, while the named engine behaviour stays.')
md_lines.append('')
md_lines.append('Counts by primary class (the first class listed); "Any E residual" counts entries whose class string names an E part:')
md_lines.append('')
md_lines.append(matrix)
md_lines.append('')
md_lines.append('No entry needs M. N/A analogs: ' + ', '.join('%s %d' % (k or 'none', v) for k, v in sorted(acnt.items())) + '.')
md_lines.append('')
md_lines.append('`catalog.tsv` has the same rows with separate evidence columns, plus `primary_class` and `source_rows` (the raw research rows in `raw/` each entry was built from).')
md_lines.append('')
for g, title in (('engine/GDScript', 'Engine and GDScript'), ('C#', 'C# (.NET) bridge')):
    md_lines.append('## ' + title)
    md_lines.append('')
    md_lines.append('| ID | Pitfall | Example | Root layer | Evidence | Frequency signal | Status | Class | Why this class |')
    md_lines.append('|---|---|---|---|---|---|---|---|---|')
    for r in out:
        if r['group'] != g:
            continue
        pit = '**' + md(r['title']) + '**<br>' + md(r['description'])
        ex = md(r['example']) if r['example'].strip() not in ('-', '') else ''
        ev = 'Issues: ' + md(r['evidence_issues']) + '<br>Docs: ' + md(r['evidence_docs']) + '<br>Source: ' + md(r['evidence_source'])
        md_lines.append('| %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            r['id'], pit, ex, md(r['root_layer']), ev, md(r['frequency']),
            md(r['status']), md(r['fix_class']), md(r['fix_reason'])))
    md_lines.append('')

with open(os.path.join(BASE, 'catalog.md'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(md_lines))

print('rows', len(out))
