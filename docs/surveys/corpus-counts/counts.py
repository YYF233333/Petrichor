"""One-off token counts over the 20-repository corpus, used by individual design decisions.

Usage: python counts.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Counts, in first-party .gd files (blank and comment lines excluded from the line total):
  - the `null` literal, split into comparisons (`== null`, `!= null`) and other uses;
  - prefix `!` (not followed by `=`) versus the `not` keyword (`not in` included);
  - `@abstract` classes and methods, and abstract classes whose methods are all abstract;
  - rest parameters (`...name`), which `...` introduces only in signatures.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    c = collections.Counter()
    by_repo = collections.defaultdict(collections.Counter)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f in repo.files:
            if not (f.endswith(".gd") and repo.first_party(f)):
                continue
            src = repo.read(f)
            c["lines"] += sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            ts = [t for t in G.tokenize(src) if t.k != "NL"]
            abstract_class = False
            funcs = abstract_funcs = 0
            for i, t in enumerate(ts):
                if t.k == "AN" and t.v == "@abstract":
                    j = i + 1
                    while j < len(ts) and ts[j].k == "AN":
                        j += 1
                    if j < len(ts) and ts[j].v in ("class_name", "extends", "class"):
                        c["abstract_class"] += 1
                        by_repo[name]["abstract_class"] += 1
                        abstract_class = abstract_class or ts[j].v != "class"
                elif t.k == "ID" and t.v == "func" and (i == 0 or ts[i - 1].line != t.line or ts[i - 1].v == "static" or ts[i - 1].k == "AN"):
                    funcs += 1  # a declaration, not a lambda
                    k = i - 1
                    while k >= 0 and (ts[k].k == "AN" or ts[k].v == "static"):
                        if ts[k].v == "@abstract":
                            c["abstract_func"] += 1
                            abstract_funcs += 1
                            break
                        k -= 1
                elif (t.k == "OP" and t.v == "." and i + 3 < len(ts) and ts[i + 1].v == "." and ts[i + 2].v == "."
                      and ts[i + 1].col == t.col + 1 and ts[i + 2].col == t.col + 2 and ts[i + 3].k == "ID"):
                    c["rest"] += 1
                    by_repo[name]["rest"] += 1
                if t.k == "ID" and t.v == "null":
                    comparing = i > 1 and ts[i - 1].v == "=" and ts[i - 2].v in ("=", "!")
                    c["null_compare" if comparing else "null_other"] += 1
                elif t.k == "ID" and t.v == "not":
                    c["not"] += 1
                elif t.k == "OP" and t.v == "!" and not (i + 1 < len(ts) and ts[i + 1].v == "=" and ts[i + 1].col == t.col + 1):
                    c["bang"] += 1
            if abstract_class and abstract_funcs and abstract_funcs == funcs:
                c["abstract_only"] += 1
    null = c["null_compare"] + c["null_other"]
    print(f"code lines: {c['lines']}")
    print(f"null: {null} ({null * 1000 / c['lines']:.1f} per 1,000 lines); "
          f"comparisons {c['null_compare']}, other uses {c['null_other']}")
    print(f"prefix !: {c['bang']}; not: {c['not']}")
    for key, label in (("abstract_class", "@abstract classes"), ("rest", "rest parameters")):
        repos = ", ".join(f"{r} {n[key]}" for r, n in sorted(by_repo.items(), key=lambda x: -x[1][key]) if n[key])
        print(f"{label}: {c[key]} ({repos})")
    print(f"@abstract methods: {c['abstract_func']}; "
          f"abstract file classes whose methods are all abstract: {c['abstract_only']}")


if __name__ == "__main__":
    main()
