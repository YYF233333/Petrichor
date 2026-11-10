"""Weak references in first-party GDScript: calls to `weakref(...)`, the name `WeakRef` and calls to `.get_ref()`.

Usage: python weak_refs.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)

Token-based (gdcorpus tokenizer), so strings and comments are skipped. `weakref(` counts without a `.` before it,
`get_ref(` only with one. Every use is listed with its repository group, and read.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

KINDS = ("weakref(", "WeakRef", ".get_ref(")


def kind(ts, i):
    t = ts[i]
    call = i + 1 < len(ts) and ts[i + 1].v == "("
    member = i > 0 and ts[i - 1].v == "."
    if t.v == "WeakRef":
        return "WeakRef"
    if t.v == "weakref" and call and not member:
        return "weakref("
    if t.v == "get_ref" and call and member:
        return ".get_ref("
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    counts = collections.defaultdict(collections.Counter)
    found = []
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f, info in G.script_infos(repo).items():
            if not info["first_party"]:
                continue
            ts = [t for t in G.tokenize(repo.read(f)) if t.k != "NL"]
            for i, t in enumerate(ts):
                k = kind(ts, i) if t.k == "ID" else None
                if k:
                    counts[k][group] += 1
                    found.append(f"{name}/{f}:{t.line} ({group}): {k}")
    for k in KINDS:
        by_group = ", ".join(f"{g} {n}" for g, n in sorted(counts[k].items()))
        print(f"{k:12} {sum(counts[k].values()):3}  {by_group}")
    for row in found:
        print(row)


if __name__ == "__main__":
    main()
