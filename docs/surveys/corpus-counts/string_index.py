"""Assignments to a character of a String (`s[i] = c`) in first-party GDScript.

Usage: python string_index.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)

Token-based (gdcorpus tokenizer, which splits `:=` and `==` into single characters). A name is a String when the
file declares it as `name: String`, `var name := "..."` or `var name = "..."`, anywhere in the file. An assignment is
a plain or compound one to `name[...]` with nothing before `name`; `==` is not one. Names typed only through calls
are missed, so the count is a lower bound; a name declared as a String in one function and as something else in
another is listed, and read.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

ASSIGNMENTS = ("=", "+=", "-=", "*=", "/=", "%=")


def string_names(ts):
    names = set()
    for i, t in enumerate(ts):
        if t.k != "ID" or i + 2 >= len(ts):
            continue
        if ts[i + 1].v == ":" and ts[i + 2].v == "String":
            names.add(t.v)
        elif i > 0 and ts[i - 1].v == "var":
            rest = [x.v for x in ts[i + 1:i + 3]]
            if (rest == [":", "="] and i + 3 < len(ts) and ts[i + 3].k == "STR") or (rest[0] == "=" and ts[i + 2].k == "STR"):
                names.add(t.v)
    return names


def closing(ts, i):
    """The index of the bracket that closes the one at ts[i]."""
    depth = 0
    for j in range(i, len(ts)):
        if ts[j].v in ("(", "[", "{"):
            depth += 1
        elif ts[j].v in (")", "]", "}"):
            depth -= 1
            if depth == 0:
                return j
    return len(ts) - 1


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    total, found = 0, []
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f, info in G.script_infos(repo).items():
            if not info["first_party"]:
                continue
            ts = [t for t in G.tokenize(repo.read(f)) if t.k != "NL"]
            strings = string_names(ts)
            for i, t in enumerate(ts):
                if t.k != "ID" or i + 1 >= len(ts) or ts[i + 1].v != "[" or (i > 0 and ts[i - 1].v == "."):
                    continue
                j = closing(ts, i + 1)
                if j + 2 >= len(ts) or ts[j + 1].v not in ASSIGNMENTS or ts[j + 2].v == "=" or ts[j + 2].line != ts[j + 1].line:
                    continue
                total += 1
                if t.v in strings:
                    value = " ".join(repr(x.v) if x.k == "STR" else str(x.v) for x in ts[j + 2:] if x.line == ts[j + 1].line)
                    found.append(f"{name}/{f}:{t.line} ({group}): {t.v}[...] {ts[j + 1].v} {value}")
    print(f"subscript assignments: {total}")
    print(f"on names declared as String: {len(found)}")
    for row in found:
        print(row)


if __name__ == "__main__":
    main()
