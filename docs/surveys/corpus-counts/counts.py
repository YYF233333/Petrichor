"""One-off token counts over the 20-repository corpus, used as evidence by individual rules of the specification.

Usage: python counts.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Counts, in first-party .gd files (blank and comment lines excluded from the line total):
  - the `null` literal, split into comparisons (`== null`, `!= null`) and other uses;
  - prefix `!` (not followed by `=`) versus the `not` keyword (`not in` included);
  - `@abstract` classes and methods, and abstract classes whose methods are all abstract;
  - rest parameters (`...name`), which `...` introduces only in signatures;
  - lambdas whose body contains `await`;
  - methods whose body contains `await` (coroutines) used as values, that is, passed as an
    argument by name in the same file, and how many of those are `connect(name)`;
  - `assert(...)` statements, and those whose arguments contain `await`;
  - `is_instance_valid` (overall and per repository, per 1,000 lines), `static var`, `_static_init`,
    `NOTIFICATION_PREDELETE`, `Thread.new` and `WorkerThreadPool`;
  - `get_parent()` called on self and `owner` read on self: member accesses on them (and the files that
    have one), and tests for null (`== null`, `!= null`, as a condition, `is_instance_valid`), including
    tests of a variable assigned from them within the next 25 lines;
  - `free()` and `set_script(...)` called on self (bare, or after `self.` or `super.`), `.free()` called
    on another object within a method whose name starts with `_on_`, with their locations for reading,
    and `queue_free()` for comparison;
  - `.pop_at(`, with locations for reading, `.pop_back(` and `.pop_front(`.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

LAMBDA_PREV = {"(", ",", "=", ":", "[", "{", "return"}
COND = {"if", "elif", "while", "not", "and", "or", "!"}
OPEN, CLOSE = ("(", "[", "{"), (")", "]", "}")


def func_body(ts, i):
    """Token range [start, end) of the body of the `func` at token i."""
    n, depth, j = len(ts), 0, i + 1
    while j < n and not (ts[j].v == ":" and depth == 0):
        depth += (ts[j].v in OPEN) - (ts[j].v in CLOSE)
        j += 1
    if j + 1 >= n:
        return n, n
    if ts[j + 1].line == ts[j].line:
        # Body on the same line: up to the end of the line or the end of the enclosing argument.
        k, depth = j + 1, 0
        while k < n and ts[k].line == ts[j].line and not (depth == 0 and (ts[k].v in CLOSE or ts[k].v == ",")):
            depth += (ts[k].v in OPEN) - (ts[k].v in CLOSE)
            k += 1
        return j + 1, k
    h = i
    while h > 0 and ts[h - 1].line == ts[i].line:
        h -= 1
    k = j + 1
    while k < n and not (ts[k].first and ts[k].col <= ts[h].col):
        k += 1
    return j + 1, k


def tested(ts, i, j):
    """Whether the expression ts[i:j] is tested for null."""
    prev = ts[i - 1].v if i > 0 else ""
    nxt = ts[j].v if j < len(ts) else ""
    if nxt in ("==", "!=") and j + 1 < len(ts) and ts[j + 1].v == "null":
        return True
    if prev in COND and nxt in (":", "and", "or", ")"):
        return True
    return prev == "(" and i > 1 and ts[i - 2].v == "is_instance_valid" and nxt == ")"


def parent_owner(ts, i, key, c, files, where):
    """Count a self `get_parent()` or `owner` whose expression is ts[i:j]."""
    j = i + 3 if key == "parent" else i + 1
    c[key] += 1
    if j < len(ts) and ts[j].v in (".", "["):
        c[key + "_member"] += 1
        files[key].add(where[0])
        return
    found = tested(ts, i, j)
    if not found and i > 1 and ts[i - 1].v == "=" and ts[i - 2].k == "ID":
        var, k = ts[i - 2].v, j
        while k < len(ts) and ts[k].line <= ts[i].line + 25 and not found:
            found = ts[k].v == var and tested(ts, k, k + 1)
            k += 1
    if found:
        c[key + "_tested"] += 1
        where[1].append(f"{where[0]}:{ts[i].line}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    c = collections.Counter()
    by_repo = collections.defaultdict(collections.Counter)
    files = {"parent": set(), "owner": set()}
    tested_at = []
    freeing_at = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f in repo.files:
            if not (f.endswith(".gd") and repo.first_party(f)):
                continue
            src = repo.read(f)
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            c["lines"] += lines
            by_repo[name]["lines"] += lines
            ts = [t for t in G.tokenize(src) if t.k != "NL"]
            abstract_class = False
            funcs = abstract_funcs = 0
            coroutines = set()
            method = ("", 0, 0)  # The method declared last: its name and body.
            for i, t in enumerate(ts):
                if (t.k == "ID" and t.v == "func" and i + 1 < len(ts) and ts[i + 1].k == "ID"
                        and (i == 0 or ts[i - 1].line != t.line or ts[i - 1].v == "static" or ts[i - 1].k == "AN")):
                    method = (ts[i + 1].v, *func_body(ts, i))
                if t.k == "ID" and t.v in ("pop_at", "pop_back", "pop_front") and i > 0 and ts[i - 1].v == "." and i + 1 < len(ts) and ts[i + 1].v == "(":
                    c[t.v] += 1
                    if t.v == "pop_at":
                        freeing_at["pop_at"].append(f"{name}/{f}:{t.line}")
                if t.k == "ID" and t.v in ("free", "set_script", "queue_free") and i + 1 < len(ts) and ts[i + 1].v == "(":
                    on_self = i == 0 or ts[i - 1].v != "." or (i > 1 and ts[i - 2].v in ("self", "super"))
                    where = f"{name}/{f}:{t.line}"
                    if t.v == "queue_free":
                        c["queue_free"] += 1
                    elif t.v == "set_script" and on_self:
                        c["set_script_self"] += 1
                        freeing_at["set_script on self"].append(where)
                    elif t.v == "free" and i + 2 < len(ts) and ts[i + 2].v == ")":
                        if on_self:
                            c["free_self"] += 1
                            freeing_at["free() on self"].append(where)
                        elif method[0].startswith("_on_") and method[1] <= i < method[2]:
                            c["free_in_handler"] += 1
                            freeing_at[".free() in an _on_ method"].append(where)
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
                    s, e = func_body(ts, i)
                    if i + 1 < len(ts) and ts[i + 1].k == "ID" and any(ts[x].v == "await" for x in range(s, e)):
                        coroutines.add(ts[i + 1].v)
                elif t.k == "ID" and t.v == "func" and ts[i - 1].v in LAMBDA_PREV:
                    c["lambda"] += 1
                    s, e = func_body(ts, i)
                    if any(ts[x].v == "await" for x in range(s, e)):
                        c["lambda_await"] += 1
                        by_repo[name]["lambda_await"] += 1
                elif (t.k == "OP" and t.v == "." and i + 3 < len(ts) and ts[i + 1].v == "." and ts[i + 2].v == "."
                      and ts[i + 1].col == t.col + 1 and ts[i + 2].col == t.col + 2 and ts[i + 3].k == "ID"):
                    c["rest"] += 1
                    by_repo[name]["rest"] += 1
                if t.k == "ID" and t.v == "assert" and t.first and i + 1 < len(ts) and ts[i + 1].v == "(":
                    c["assert"] += 1
                    by_repo[name]["assert"] += 1
                    j, depth = i + 1, 0
                    while j < len(ts):
                        depth += (ts[j].v in OPEN) - (ts[j].v in CLOSE)
                        if ts[j].v == "await":
                            c["assert_await"] += 1
                            break
                        if depth == 0:
                            break
                        j += 1
                if t.k == "ID":
                    nxt = ts[i + 1].v if i + 1 < len(ts) else None
                    if t.v == "is_instance_valid" and nxt == "(":
                        c["is_instance_valid"] += 1
                        by_repo[name]["is_instance_valid"] += 1
                    elif t.v == "static" and nxt == "var":
                        c["static_var"] += 1
                    elif t.v == "_static_init" and i > 0 and ts[i - 1].v == "func":
                        c["static_init"] += 1
                    elif t.v == "NOTIFICATION_PREDELETE":
                        c["predelete"] += 1
                        by_repo[name]["predelete"] += 1
                    elif t.v == "Thread" and nxt == "." and i + 2 < len(ts) and ts[i + 2].v == "new":
                        c["thread_new"] += 1
                    elif t.v == "WorkerThreadPool":
                        c["worker_pool"] += 1
                    if i == 0 or ts[i - 1].v != ".":
                        if t.v == "get_parent" and nxt == "(" and i + 2 < len(ts) and ts[i + 2].v == ")":
                            parent_owner(ts, i, "parent", c, files, (f"{name}/{f}", tested_at))
                        elif t.v == "owner" and nxt not in ("=", ":") and ts[i - 1].v not in ("var", "func", "(", ","):
                            parent_owner(ts, i, "owner", c, files, (f"{name}/{f}", tested_at))
                if t.k == "ID" and t.v == "null":
                    comparing = i > 1 and ts[i - 1].v == "=" and ts[i - 2].v in ("=", "!")
                    c["null_compare" if comparing else "null_other"] += 1
                elif t.k == "ID" and t.v == "not":
                    c["not"] += 1
                elif t.k == "OP" and t.v == "!" and not (i + 1 < len(ts) and ts[i + 1].v == "=" and ts[i + 1].col == t.col + 1):
                    c["bang"] += 1
            if abstract_class and abstract_funcs and abstract_funcs == funcs:
                c["abstract_only"] += 1
            for i, t in enumerate(ts):
                if (t.k == "ID" and t.v in coroutines and 0 < i < len(ts) - 1
                        and ts[i - 1].v in ("(", ",") and ts[i + 1].v in (")", ",", ".")):
                    c["coroutine_value"] += 1
                    by_repo[name]["coroutine_value"] += 1
                    if ts[i - 1].v == "(" and i > 1 and ts[i - 2].v == "connect":
                        c["coroutine_connect"] += 1
    null = c["null_compare"] + c["null_other"]
    print(f"code lines: {c['lines']}")
    print(f"null: {null} ({null * 1000 / c['lines']:.1f} per 1,000 lines); "
          f"comparisons {c['null_compare']}, other uses {c['null_other']}")
    print(f"prefix !: {c['bang']}; not: {c['not']}")
    for key, label in (("abstract_class", "@abstract classes"), ("rest", "rest parameters"),
                       ("lambda_await", "lambdas containing await"), ("coroutine_value", "coroutine methods used as values"),
                       ("assert", "assert statements")):
        repos = ", ".join(f"{r} {n[key]}" for r, n in sorted(by_repo.items(), key=lambda x: (-x[1][key], x[0])) if n[key])
        print(f"{label}: {c[key]} ({repos})")
    print(f"@abstract methods: {c['abstract_func']}; "
          f"abstract file classes whose methods are all abstract: {c['abstract_only']}")
    print(f"lambdas: {c['lambda']}; coroutine methods passed to connect(name): {c['coroutine_connect']}")
    print(f"assert statements whose arguments contain await: {c['assert_await']}")
    per_k = ", ".join(f"{r} {n['is_instance_valid'] * 1000 / n['lines']:.1f}"
                      for r, n in sorted(by_repo.items(), key=lambda x: -x[1]["is_instance_valid"] / max(x[1]["lines"], 1))
                      if n["is_instance_valid"])
    print(f"is_instance_valid: {c['is_instance_valid']} ({c['is_instance_valid'] * 1000 / c['lines']:.1f} per 1,000 lines); "
          f"per repository per 1,000 lines: {per_k}")
    predelete = ", ".join(f"{r} {n['predelete']}" for r, n in sorted(by_repo.items(), key=lambda x: -x[1]["predelete"]) if n["predelete"])
    print(f"NOTIFICATION_PREDELETE: {c['predelete']} ({predelete})")
    print(f"static var: {c['static_var']}; _static_init: {c['static_init']}; "
          f"Thread.new: {c['thread_new']}; WorkerThreadPool: {c['worker_pool']}")
    for key, label in (("parent", "get_parent() on self"), ("owner", "owner on self")):
        print(f"{label}: {c[key]}; member accesses {c[key + '_member']} in {len(files[key])} files; "
              f"tested for null {c[key + '_tested']}")
    print(f"tested for null at: {', '.join(tested_at)}")
    print(f"free() on self: {c['free_self']}; set_script on self: {c['set_script_self']}; "
          f".free() in an _on_ method: {c['free_in_handler']}; queue_free(): {c['queue_free']}")
    print(f"pop_at: {c['pop_at']}; pop_back: {c['pop_back']}; pop_front: {c['pop_front']}")
    for label, places in freeing_at.items():
        print(f"{label} at: {', '.join(places)}")


if __name__ == "__main__":
    main()
