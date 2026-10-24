"""One-off token counts over the 20-repository corpus, used by individual design decisions.

Usage: python counts.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Counts, in first-party .gd files (blank and comment lines excluded from the line total):
  - the `null` literal, split into comparisons (`== null`, `!= null`) and other uses;
  - prefix `!` (not followed by `=`) versus the `not` keyword (`not in` included).
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
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f in repo.files:
            if not (f.endswith(".gd") and repo.first_party(f)):
                continue
            src = repo.read(f)
            c["lines"] += sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            ts = [t for t in G.tokenize(src) if t.k != "NL"]
            for i, t in enumerate(ts):
                if t.k == "ID" and t.v == "null":
                    comparing = i > 1 and ts[i - 1].v == "=" and ts[i - 2].v in ("=", "!")
                    c["null_compare" if comparing else "null_other"] += 1
                elif t.k == "ID" and t.v == "not":
                    c["not"] += 1
                elif t.k == "OP" and t.v == "!" and not (i + 1 < len(ts) and ts[i + 1].v == "=" and ts[i + 1].col == t.col + 1):
                    c["bang"] += 1
    null = c["null_compare"] + c["null_other"]
    print(f"code lines: {c['lines']}")
    print(f"null: {null} ({null * 1000 / c['lines']:.1f} per 1,000 lines); "
          f"comparisons {c['null_compare']}, other uses {c['null_other']}")
    print(f"prefix !: {c['bang']}; not: {c['not']}")


if __name__ == "__main__":
    main()
