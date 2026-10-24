"""Print the counts reported in property-accessors.{en,zh}.md.

Usage: python count.py CORPUS_DIR

Reads CORPUS_DIR/properties.json and CORPUS_DIR/corpus.json (written by extract.py) and
classification.json (next to this script, the manual classification). Unless stated
otherwise, counts are over first-party code without tests (see scope.py).

Getter categories:
  a  the getter only returns the backing field (removable under the split)
  b  the getter reads the backing field and does more (needs rewriting under the split)
  c  the getter does not read the backing field (a computed property)
A getter whose body is literally `return <field>` is category a without a manual entry;
every other getter has an entry in classification.json.
"""
import collections
import json
import os
import re
import sys

sys.dont_write_bytecode = True
from scope import scope_of  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ORDER = ['godot-demo-projects', 'godot-open-rpg', 'Starter-Kit-3D-Platformer',
              'Starter-Kit-City-Builder', 'Starter-Kit-FPS', 'Starter-Kit-Racing', 'tps-demo',
              'Pixelorama', 'phantom-camera', 'godot_dialogue_manager', 'beehave',
              'godot-statecharts', 'gdUnit4', 'Godot-Game-Template']


def table(header, rows):
    print('| ' + ' | '.join(header) + ' |')
    print('|' + '---|' * len(header))
    for r in rows:
        print('| ' + ' | '.join(str(x) for x in r) + ' |')
    print()


def per_k(n, lines):
    return f'{1000 * n / lines:.2f}' if lines else '-'


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    root = sys.argv[1]
    with open(os.path.join(root, 'properties.json'), encoding='utf-8') as f:
        props = json.load(f)
    with open(os.path.join(root, 'corpus.json'), encoding='utf-8') as f:
        corpus = json.load(f)
    with open(os.path.join(HERE, 'classification.json'), encoding='utf-8') as f:
        cls = json.load(f)

    # Check that the manual data matches the extraction.
    ids = {p['id']: p for p in props}
    for section in ('getters', 'setters', 'setter_notes', 'hazards'):
        for k in cls[section]:
            assert k in ids, f'{section}: unknown id {k}'
    for p in props:
        if p['scope'] not in ('first-party', 'test'):
            continue
        if p['getter'] and not p['getter'].get('trivial'):
            assert p['id'] in cls['getters'], f'unclassified getter {p["id"]}'
        if p['setter']:
            assert p['id'] in cls['setters'], f'unclassified setter {p["id"]}'

    def gcat(p):
        if not p['getter']:
            return None
        if p['getter'].get('trivial'):
            return 'a'
        return cls['getters'][p['id']]['cat']

    fp = [p for p in props if p['scope'] == 'first-party']
    test = [p for p in props if p['scope'] == 'test']
    fp_lines = sum(c['first-party']['lines'] for c in corpus.values())
    test_lines = sum(c['test']['lines'] for c in corpus.values())

    print('## Corpus\n')
    rows = []
    for repo in REPO_ORDER:
        c = corpus[repo]
        n = sum(1 for p in fp if p['repo'] == repo)
        rows.append([repo, c['commit'], c['files'], f"{c['lines']:,}", f"{c['first-party']['lines']:,}",
                     f"{c['test']['lines']:,}" if c['test']['lines'] else '-', n])
    rows.append(['Total', '', sum(c['files'] for c in corpus.values()),
                 f"{sum(c['lines'] for c in corpus.values()):,}", f'{fp_lines:,}', f'{test_lines:,}', len(fp)])
    table(['repo', 'commit', '.gd files', 'lines', 'first-party lines', 'test lines', 'properties'], rows)
    print('excluded lines: vendored', sum(c['vendored']['lines'] for c in corpus.values()),
          'duplicate', sum(c['duplicate']['lines'] for c in corpus.values()),
          '| properties in vendored code', sum(1 for p in props if p['scope'] == 'vendored'),
          'duplicate', sum(1 for p in props if p['scope'] == 'duplicate'))
    print('Godot 3 setget occurrences:', sum(c['setget'] for c in corpus.values()))
    print()

    print('## Overall (first-party, no tests)\n')
    shape = collections.Counter(('setter' if p['setter'] else '') + ('+getter' if p['getter'] else '')
                                for p in fp)
    print(f'properties with accessors: {len(fp)} ({per_k(len(fp), fp_lines)} per 1,000 lines)')
    print('shape:', dict(shape))
    print('exported:', sum(p['export'] for p in fp), '| static:', sum(p['static'] for p in fp),
          '| @onready:', sum(p['onready'] for p in fp), '| with initializer:',
          sum(p['init'] is not None for p in fp))
    print('form:', dict(collections.Counter(p['form'] for p in fp)))
    print()

    print('## Getters by category\n')
    rows = []
    getters = [p for p in fp if p['getter']]
    for cat in ('a', 'b', 'c'):
        sel = [p for p in getters if gcat(p) == cat]
        rows.append([cat, len(sel), sum(p['export'] for p in sel), sum(1 for p in sel if p['setter']),
                     per_k(len(sel), fp_lines)])
    rows.append(['all', len(getters), sum(p['export'] for p in getters),
                 sum(1 for p in getters if p['setter']), per_k(len(getters), fp_lines)])
    table(['category', 'getters', 'exported', 'with setter', 'per 1,000 lines'], rows)
    manual = [p for p in getters + [p for p in test if p['getter']] if not p['getter'].get('trivial')]
    agree = sum(1 for p in manual if p['getter']['uses_field'] == (gcat(p) != 'c'))
    print(f'mechanical test "the getter mentions its own field" agrees with the manual a/b vs c '
          f'decision for {agree} of {len(manual)} non-trivial getters')
    a_manual = [p for p in getters if gcat(p) == 'a' and not p['getter'].get('trivial')]
    print('category a written non-trivially:', [p['id'] for p in a_manual])
    a_named = sum(1 for p in getters if gcat(p) == 'a' and p['getter']['kind'] == 'named')
    print('category a by form: named', a_named, 'inline', sum(1 for p in getters if gcat(p) == 'a') - a_named)
    print('category a by repo:', dict(collections.Counter(p['repo'] for p in getters if gcat(p) == 'a')))
    print()

    print('## Category b (every case)\n')
    for p in getters:
        if gcat(p) == 'b':
            e = cls['getters'][p['id']]
            print(f"- {p['id']} `{p['name']}`{' (exported)' if p['export'] else ''}: {e['what']}"
                  + (f" [{e['early']}]" if e.get('early') else ''))
    print()

    print('## Category c\n')
    cs = [p for p in getters if gcat(p) == 'c']
    kinds = collections.Counter(cls['getters'][p['id']]['kind'] for p in cs)
    print('by kind:', dict(kinds))
    print('with setter:', sum(1 for p in cs if p['setter']), '| setter writes own field:',
          [p['id'] for p in cs if p['setter'] and p['setter'].get('writes_field')])
    print('exported:', [p['id'] for p in cs if p['export']])
    print('with initializer (storage never read):', [p['id'] for p in cs if p['init'] is not None])
    print('by repo:', dict(collections.Counter(p['repo'] for p in cs)))
    for p in cs:
        e = cls['getters'][p['id']]
        print(f"- {p['id']} `{p['name']}` [{e['kind']}]{' (exported)' if p['export'] else ''}"
              f"{' setter' if p['setter'] else ' read-only'}: {e['what']}")
    print()

    print('## Getters that can fail or act when read early\n')
    for p in getters:
        e = cls['getters'].get(p['id'])
        if e and e.get('early'):
            print(f"- {p['id']} [{e['cat']}]{' (exported)' if p['export'] else ''}: {e['early']}")
    print()

    print('## Setters\n')
    setters = [p for p in fp if p['setter']]
    rows = []
    for label, sel in (('setter only', [p for p in setters if not p['getter']]),
                       ('setter and getter', [p for p in setters if p['getter']]),
                       ('all setters', setters)):
        k = collections.Counter(cls['setters'][p['id']] for p in sel)
        rows.append([label, len(sel), k['plain'], k['effect'], k['validate'], k['proxy'],
                     sum(p['export'] for p in sel)])
    table(['properties', 'n', 'plain', 'effect', 'validate', 'proxy', 'exported'], rows)
    print('setter-only proxies:', [(p['id'], cls['setter_notes'].get(p['id'], '')) for p in setters
                                    if not p['getter'] and cls['setters'][p['id']] == 'proxy'])
    print('setter-only by export:', dict(collections.Counter(p['export'] for p in setters if not p['getter'])))
    aw = [p for p in setters if any(re.search(r'\bawait\b', s) for s in p['setter']['stmts'])]
    aw_ready = [p for p in aw if any(re.search(r'\bawait\s+(?:self\.)?ready\b', s) for s in p['setter']['stmts'])]
    print(f'setters containing await (coroutine setters, not allowed by spec §4.4): {len(aw)}, exported '
          f'{sum(p["export"] for p in aw)}; awaiting `ready`: {len(aw_ready)}; by repo',
          dict(collections.Counter(p['repo'] for p in aw)))
    print('getters containing await:',
          sum(1 for p in fp if p['getter'] and any(re.search(r'\bawait\b', s) for s in p['getter']['stmts'])))
    print()

    print('## Setter runs before _ready (exported setters)\n')
    exp_set = [p for p in fp if p['setter'] and p['export']]
    hz = [p for p in exp_set if p['id'] in cls['hazards']]
    print('exported properties with a setter:', len(exp_set),
          '| reaching a child node, @onready member or member assigned in _ready (reviewed):', len(hz))
    v = collections.Counter(cls['hazards'][p['id']]['verdict'] for p in hz)
    print('verdicts:', dict(v))
    by_file = collections.defaultdict(list)
    for p in hz:
        if cls['hazards'][p['id']]['verdict'] == 'unguarded':
            by_file[f"{p['repo']}/{p['file']}"].append(p['name'])
    for f_, names in by_file.items():
        print(f'- unguarded: {f_}: {len(names)} ({", ".join(names)})')
    for p in hz:
        h = cls['hazards'][p['id']]
        if h['verdict'] not in ('guarded', 'unguarded'):
            print(f"- {h['verdict']}: {p['id']} `{p['name']}`: {h['note']}")
    guards = collections.Counter(cls['hazards'][p['id']]['guard'] for p in hz
                                 if cls['hazards'][p['id']]['verdict'] == 'guarded')
    print('guards used:', dict(guards))
    onr = [p for p in hz if any(t.startswith('onready') for t in p['setter'].get('touches', []))]
    print('reaching an @onready member:', len(onr),
          dict(collections.Counter(cls['hazards'][p['id']]['verdict'] for p in onr)))
    child = [p for p in hz if any(t.startswith('child') for t in p['setter'].get('touches', []))]
    print('reaching a child node ($, %, get_node, get_child...):', len(child),
          dict(collections.Counter(cls['hazards'][p['id']]['verdict'] for p in child)))
    non_exp = [p for p in fp if p['setter'] and not p['export']
               and any(not t.startswith('tree') for t in p['setter'].get('touches', []))]
    print('non-exported setters reaching such members (not reviewed for guards):', len(non_exp))
    reapplied = [p for p in fp if p['setter'] and '_ready' in (p.get('reapplied_in') or [])]
    print('properties re-assigned to themselves in _ready (`x = x`):', len(reapplied),
          dict(collections.Counter(p['repo'] for p in reapplied)))
    print()

    print('## Named accessors overridden in a subclass\n')
    for kind in ('setter', 'getter'):
        named = [p for p in fp if p[kind] and p[kind]['kind'] == 'named']
        over = []
        for p in named:
            files = [f for f in p[kind].get('overridden_in', [])
                     if scope_of(p['repo'], f) == 'first-party']
            if files:
                over.append((p['id'], p[kind]['func'], files))
        print(f'named {kind}s: {len(named)}; overridden in a first-party subclass: {len(over)}', over)
    print()

    print('## Per repository\n')
    rows = []
    for repo in REPO_ORDER:
        sel = [p for p in fp if p['repo'] == repo]
        if not sel:
            continue
        lines = corpus[repo]['first-party']['lines']
        cat = collections.Counter(gcat(p) for p in sel if p['getter'])
        rows.append([repo, len(sel), per_k(len(sel), lines), sum(1 for p in sel if not p['getter']),
                     cat['a'], cat['b'], cat['c'], sum(p['export'] for p in sel)])
    table(['repo', 'properties', 'per 1,000 lines', 'setter only', 'a', 'b', 'c', 'exported'], rows)

    print('## Tests\n')
    print('test properties:', len(test), [(p['id'], gcat(p), cls['setters'].get(p['id'])) for p in test])


main()
