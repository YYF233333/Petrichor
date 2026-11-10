"""Calls to the engine APIs that return null on failure with an error read afterwards, in first-party GDScript.

Usage: python null_failures.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)

Token-based (gdcorpus tokenizer, which splits `:=` and `==` into single characters). A call is `Class.method(`.
Within the call's line and the next 3 lines, a call is counted as:
- error: `get_open_error` appears;
- null: otherwise, the result is tested, directly or through the name it is assigned to on that line. A test is
  the value alone, after `not` or `!`, compared with `null`, inside `assert(...)` or `is_instance_valid(...)`, or
  calling `is_empty()` (get_file_as_string/bytes return an empty value on failure);
- none: neither.
A check further down is missed.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

APIS = {
    "FileAccess": ("open", "open_encrypted", "open_encrypted_with_pass", "open_compressed", "create_temp",
                   "get_file_as_string", "get_file_as_bytes"),
    "DirAccess": ("open", "create_temp"),
}
WINDOW = 3
KINDS = ("error", "null", "none")


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


def tested(ts, start, end):
    """Whether the value at ts[start..end] is tested for failure."""
    before = ts[start - 1].v if start > 0 else None
    after = [t.v for t in ts[end + 1:end + 4]]
    if after[:3] in (["=", "=", "null"], ["!", "=", "null"]) or after[:2] == [".", "is_empty"]:
        return True
    if start >= 3 and [t.v for t in ts[start - 3:start]] in (["null", "=", "="], ["null", "!", "="]):
        return True
    if after[:1] == ["."]:
        return False
    return before in ("if", "elif", "while", "not", "!", "and", "or") or (
        before == "(" and start >= 2 and ts[start - 2].v in ("assert", "is_instance_valid"))


def assigned_name(ts, first, i):
    """The name the call at ts[i] is assigned to, when the statement starting at ts[first] is `[var] name ... = call`."""
    if i < 1 or ts[i - 1].v != "=" or ts[i - 2].v in ("=", "!", "<", ">"):
        return None
    if ts[first].v == "var":
        return ts[first + 1].v
    return ts[first].v if first + 1 == i - 1 and ts[first].k == "ID" else None


def classify(ts, i, end, first):
    line = ts[i].line
    last = first
    while last + 1 < len(ts) and ts[last + 1].line <= line + WINDOW:
        last += 1
    window = range(first, last + 1)
    if any(ts[k].v == "get_open_error" for k in window):
        return "error"
    if tested(ts, i, end):
        return "null"
    name = assigned_name(ts, first, i)
    if name and any(ts[k].line > line and ts[k].v == name and ts[k - 1].v != "." and tested(ts, k, k) for k in window):
        return "null"
    return "none"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    calls = collections.defaultdict(collections.Counter)
    repos = collections.defaultdict(set)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f, info in G.script_infos(repo).items():
            if not info["first_party"]:
                continue
            ts = [t for t in G.tokenize(repo.read(f)) if t.k != "NL"]
            first = 0
            for i, t in enumerate(ts):
                if t.first:
                    first = i
                if t.v not in APIS or i + 3 >= len(ts) or ts[i + 1].v != "." or ts[i + 3].v != "(":
                    continue
                if ts[i + 2].v not in APIS[t.v] or (i > 0 and ts[i - 1].v == "."):
                    continue
                api = f"{t.v}.{ts[i + 2].v}"
                kind = classify(ts, i, closing(ts, i + 3), first)
                calls[api][kind] += 1
                calls[api][group] += 1
                calls[api][f"{group} {kind}"] += 1
                repos[api].add(name)
    groups = sorted({g for *_, g in G.REPOS})
    apis = [f"{cls}.{m}" for cls, methods in APIS.items() for m in methods]
    print(f"{'api':36} calls repos " + " ".join(f"{k:>5}" for k in KINDS))
    for api in apis:
        c = calls[api]
        print(f"{api:36} {sum(c[k] for k in KINDS):5} {len(repos[api]):5} " + " ".join(f"{c[k]:5}" for k in KINDS))
    print()
    print("by group (calls: error/null/none)")
    for api in apis:
        parts = [f"{g} {calls[api][g]}: " + "/".join(str(calls[api][f'{g} {k}']) for k in KINDS) for g in groups if calls[api][g]]
        if parts:
            print(f"{api:36} " + ", ".join(parts))


if __name__ == "__main__":
    main()
