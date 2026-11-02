"""One-off token counts over the 20-repository corpus, used as evidence by individual rules of the specification.

Usage: python counts.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Counts, in first-party .gd files (blank and comment lines excluded from the line total):
  - the `null` literal, split into comparisons (`== null`, `!= null`) and other uses;
  - prefix `!` (not followed by `=`) versus the `not` keyword (`not in` included);
  - `@abstract` classes and methods, and abstract classes whose methods are all abstract;
  - rest parameters (`...name`), which `...` introduces only in signatures;
  - lambdas whose body contains `await`; lambdas that are named or have default values; and of the lambdas
    in methods, those that assign a parameter or local the method declares before them, directly or
    through members and subscripts, with locations of the latter for reading;
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
  - `.pop_at(`, with locations for reading, `.pop_back(` and `.pop_front(`;
  - strings in single or double quotes, not tripled, that contain a line break, by repository,
    with locations for reading;
  - files with `@tool`, by group of repository and by game repository, and those of them that name
    an editor plugin class (EditorPlugin, EditorInspectorPlugin, EditorImportPlugin, EditorExportPlugin,
    EditorScript);
  - `preload` of a literal `.gd` path: into a constant, followed by `.new()` or another member, or as a
    value; and in the same file the uses of such constants: `.new()`, other calls, other members, after
    `is`, as a type (after `:`, `->`, `as` or in `Array[...]`), and as values;
  - enum declarations, named and unnamed, and those whose values, all int literals or counted on, do not
    include 0; casts with `as` to an enum the repository declares, written by its name or after a
    class_name of the repository; `@warning_ignore("int_as_enum_without_cast")`; such enum names used as a
    Dictionary (`keys()`, `values()`, `size()`, `get()`, `find_key()`, `has()`, `Name[...]`,
    `for x in Name:`), and constants initialized with one; match statements with a branch on the engine's
    KEY_, MOUSE_BUTTON_, JOY_BUTTON_ or JOY_AXIS_ constants and no `_` branch.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

LAMBDA_PREV = {"(", ",", "=", ":", "[", "{", "return"}
EDITOR_PLUGINS = {"EditorPlugin", "EditorInspectorPlugin", "EditorImportPlugin", "EditorExportPlugin", "EditorScript"}
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


def params(ts, i):
    """The parameter names of the `func` at token i, and whether one has a default value."""
    j = i + 1
    while j < len(ts) and ts[j].v != "(":
        j += 1
    names, default, depth, start = [], False, 0, True
    for t in ts[j + 1:]:
        if depth == 0 and t.v == ")":
            break
        depth += (t.v in OPEN) - (t.v in CLOSE)
        if depth == 0 and t.v == ",":
            start = True
        elif depth == 0 and start and t.k == "ID":
            names.append(t.v)
            start = False
        default = default or (depth == 0 and t.v == "=")
    return names, default


def assigns(ts, k):
    """Whether the token after ts[k] begins an assignment: `=` or a compound operator such as `+=`."""
    if k + 1 >= len(ts):
        return False
    a, b = ts[k + 1], ts[k + 2] if k + 2 < len(ts) else None
    adjacent = b is not None and b.v == "=" and b.col == a.col + 1
    if a.v == "=":
        return not adjacent
    return a.k == "OP" and a.v in "+-*/%&|^" and adjacent and not (k + 3 < len(ts) and ts[k + 3].v == "=" and ts[k + 3].col == b.col + 1)


def lambda_captures(ts, i, s, e, method, c, where):
    """Count a lambda at token i, with body ts[s:e], that is named, has default values or assigns a local of
    the method around it: the local itself, or through members and subscripts."""
    names, default = params(ts, i)
    c["lambda_named"] += ts[i + 1].k == "ID"
    c["lambda_default"] += default
    if not method[1] <= i < method[2]:
        return
    c["lambda_in_method"] += 1
    declared = lambda a, b: {ts[k + 1].v for k in range(a, b - 1) if ts[k].v in ("var", "for") and ts[k + 1].k == "ID"}
    captured = (set(params(ts, method[3])[0]) | declared(method[1], i)) - set(names) - declared(s, e)
    direct = through = False
    for k in range(s, e):
        if ts[k].k != "ID" or ts[k].v not in captured or ts[k - 1].v in (".", "$", "%"):
            continue
        m = k + 1
        while m < e and ts[m].v in (".", "["):
            if ts[m].v == ".":
                m += 2
                continue
            depth = 0
            while m < e:
                depth += (ts[m].v == "[") - (ts[m].v == "]")
                m += 1
                if depth == 0:
                    break
        if assigns(ts, m - 1):
            direct, through = direct or m == k + 1, through or m > k + 1
            if m > k + 1:
                where[1].append(f"{where[0]}:{ts[k].line} {ts[k].v}")
    c["lambda_capture_assign"] += direct
    c["lambda_capture_member"] += through


def multiline_strings(src):
    """The lines where strings in single or double quotes, not tripled, that contain a line break begin."""
    found, i, n, line = [], 0, len(src), 1
    while i < n:
        c = src[i]
        if c == "#":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c not in "\"'":
            line += c == "\n"
            i += 1
            continue
        # A raw string keeps a backslash, which still keeps a quote or a backslash after it from ending it.
        raw = i > 0 and src[i - 1] == "r" and (i < 2 or not (src[i - 2].isalnum() or src[i - 2] == "_"))
        triple = src[i:i + 3] == c * 3
        start, spans = line, False
        i += 3 if triple else 1
        while i < n:
            if src[i] == "\\" and (not raw or src[i + 1:i + 2] in (c, "\\")):
                line += src[i + 1:i + 2] == "\n"
                i += 2
                continue
            if src[i] == "\n":
                line += 1
                spans = spans or not triple
            if src[i:i + (3 if triple else 1)] == c * (3 if triple else 1):
                i += 3 if triple else 1
                break
            i += 1
        if spans:
            found.append(start)
    return found


def script_preloads(ts, c, repo, files, where):
    """Count the preloads of .gd files in a file, and the uses of the constants they initialize."""
    consts = set()
    for i, t in enumerate(ts):
        if not (t.k == "ID" and t.v == "preload" and i + 3 < len(ts) and ts[i + 1].v == "(" and ts[i + 2].k == "STR"
                and ts[i + 2].v.endswith(".gd") and ts[i + 3].v == ")"):
            continue
        c["preload_gd"] += 1
        c["preload_gd_" + where[1]] += 1
        repo["preload_gd"] += 1
        # `const X = preload(…)`, `const X := preload(…)` or `const X: T = preload(…)`.
        j = i - 1
        while j > 0 and ts[j].line == t.line and ts[j - 1].v != "const":
            j -= 1
        if j > 0 and ts[j - 1].v == "const" and ts[j].k == "ID" and ts[i - 1].v == "=":
            c["preload_gd_const"] += 1
            consts.add(ts[j].v)
        elif i + 5 < len(ts) and ts[i + 4].v == "." and ts[i + 5].v == "new":
            c["preload_gd_new"] += 1
        elif i + 4 < len(ts) and ts[i + 4].v == ".":
            c["preload_gd_member"] += 1
        else:
            c["preload_gd_value"] += 1
    for i, t in enumerate(ts):
        # Not a member of the same name, the declaration itself, or a node path such as $Name or %Name.
        if t.k != "ID" or t.v not in consts or (i > 0 and ts[i - 1].v in (".", "const", "$", "%")):
            continue
        prev = ts[i - 1].v if i > 0 else ""
        if i + 2 < len(ts) and ts[i + 1].v == ".":
            call = i + 3 < len(ts) and ts[i + 3].v == "("
            key = "new" if call and ts[i + 2].v == "new" else "static_call" if call else "member"
            c["alias_" + key] += 1
            files[key].add(where[0])
        elif prev == "is":
            c["alias_is"] += 1
        elif prev in (":", ">", "as") or (prev == "[" and i > 1 and ts[i - 2].v == "Array"):
            c["alias_type"] += 1
        else:
            c["alias_value"] += 1


ENUM_DICT_METHODS = {"keys", "values", "size", "get", "find_key", "has"}
ENGINE_ENUM_PREFIXES = ("KEY_", "MOUSE_BUTTON_", "JOY_BUTTON_", "JOY_AXIS_")


def enum_declarations(ts):
    """The enums a file declares: (name or None, values, or None for a value that is not an int literal)."""
    found = []
    for i, t in enumerate(ts):
        if not (t.k == "ID" and t.v == "enum"):
            continue
        j = i + 1
        name = ts[j].v if j < len(ts) and ts[j].k == "ID" else None
        j += name is not None
        if j >= len(ts) or ts[j].v != "{":
            continue
        values, previous, depth, k, member, written = [], -1, 0, j, None, []

        def close():
            nonlocal previous
            if member is None:
                return
            if written:
                try:
                    previous = int("".join(written), 0)
                except ValueError:
                    previous = None
            else:
                previous = None if previous is None else previous + 1
            values.append(previous)

        while k < len(ts):
            v = ts[k].v
            depth += (v in OPEN) - (v in CLOSE)
            if depth == 0:
                close()
                break
            if depth == 1 and v == ",":
                close()
                member, written = None, []
            elif depth == 1 and member is None and ts[k].k == "ID":
                member = v
            elif member is not None and v not in ("=", "{"):
                written.append(v)
            k += 1
        found.append((name, values))
    return found


def enum_uses(ts, enums, classes, c, where):
    """Count the uses of the enums of a repository in a file: casts with `as`, uses of the name as a Dictionary,
    and constants that alias an enum. `Q.Name` counts when Q is a class_name of the repository."""
    for i, t in enumerate(ts):
        if t.k != "ID" or t.v not in enums or (i > 0 and ts[i - 1].v == "enum"):
            continue
        start = i
        if i > 1 and ts[i - 1].v == ".":
            if ts[i - 2].v not in classes:
                continue
            start = i - 2
        elif i > 0 and ts[i - 1].v == ".":
            continue
        prev = ts[start - 1].v if start > 0 else ""
        nxt = ts[i + 1].v if i + 1 < len(ts) else ""
        if prev == "as":
            c["enum_as"] += 1
            where.append(f"{ts[i].line} as {t.v}")
        elif nxt == "." and i + 3 < len(ts) and ts[i + 2].v in ENUM_DICT_METHODS and ts[i + 3].v == "(":
            c["enum_dict"] += 1
            c["enum_dict_" + ts[i + 2].v] += 1
        elif nxt == "[":
            c["enum_dict"] += 1
            c["enum_dict_index"] += 1
        elif prev == "in" and start > 2 and ts[start - 3].v == "for" and nxt == ":":
            c["enum_dict"] += 1
            c["enum_dict_for"] += 1
        elif prev == "=" and start > 2 and ts[start - 3].v == "const" and (i + 1 >= len(ts) or ts[i + 1].line != t.line):
            c["enum_alias"] += 1


def engine_enum_matches(src, c, where):
    """Count the match statements whose branches test the engine's enum constants and have no `_` branch."""
    lines = src.splitlines()
    for i, line in enumerate(lines):
        if not line.strip().startswith("match ") or not line.rstrip().endswith(":"):
            continue
        indent = len(line[:len(line) - len(line.lstrip())].expandtabs(4))
        patterns, branch = [], None
        for k in range(i + 1, len(lines)):
            text = lines[k]
            if not text.strip() or text.strip().startswith("#"):
                continue
            width = len(text[:len(text) - len(text.lstrip())].expandtabs(4))
            if width <= indent:
                break
            branch = width if branch is None else branch
            if width == branch:
                patterns.append(text.strip().split(":")[0].strip())
        if any(p.startswith(ENGINE_ENUM_PREFIXES) for p in patterns) and not any(p == "_" for p in patterns):
            c["engine_enum_match"] += 1
            where.append(i + 1)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    c = collections.Counter()
    by_repo = collections.defaultdict(collections.Counter)
    files = {"parent": set(), "owner": set(), "new": set(), "static_call": set(), "member": set()}
    tested_at = []
    freeing_at = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        repo_files = []
        for f in repo.files:
            if not (f.endswith(".gd") and repo.first_party(f)):
                continue
            src = repo.read(f)
            for line in multiline_strings(src):
                by_repo[name]["multiline_string"] += 1
                freeing_at["string with a line break"].append(f"{name}/{f}:{line}")
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            c["lines"] += lines
            by_repo[name]["lines"] += lines
            ts = [t for t in G.tokenize(src) if t.k != "NL"]
            repo_files.append((f, ts))
            for ename, values in enum_declarations(ts):
                c["enum"] += 1
                c["enum_named" if ename else "enum_unnamed"] += 1
                if None not in values and 0 not in values:
                    c["enum_without_zero"] += 1
                    freeing_at["enum without the value 0"].append(f"{name}/{f} {ename}")
            for i, t in enumerate(ts):
                if t.k == "AN" and t.v.startswith("@warning_ignore") and i + 2 < len(ts) and ts[i + 1].v == "(":
                    k = i + 2
                    while k < len(ts) and ts[k].v != ")":
                        c["enum_ignore"] += ts[k].k == "STR" and ts[k].v == "int_as_enum_without_cast"
                        k += 1
            match_lines = []
            engine_enum_matches(src, c, match_lines)
            freeing_at["match over the engine's enums without _"] += [f"{name}/{f}:{line}" for line in match_lines]
            script_preloads(ts, c, by_repo[name], files, (f"{name}/{f}", group))
            if any(t.k == "AN" and t.v == "@tool" for t in ts):
                c["tool_" + group] += 1
                by_repo[name]["tool"] += 1
                c["tool_editor_api"] += any(t.k == "ID" and t.v in EDITOR_PLUGINS for t in ts)
            abstract_class = False
            funcs = abstract_funcs = 0
            coroutines = set()
            method = ("", 0, 0, 0)  # The method declared last: its name, its body and its `func`.
            for i, t in enumerate(ts):
                if (t.k == "ID" and t.v == "func" and i + 1 < len(ts) and ts[i + 1].k == "ID"
                        and (i == 0 or ts[i - 1].line != t.line or ts[i - 1].v == "static" or ts[i - 1].k == "AN")):
                    method = (ts[i + 1].v, *func_body(ts, i), i)
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
                    lambda_captures(ts, i, s, e, method, c, (f"{name}/{f}", freeing_at["assignment through a capture"]))
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
        enums = {e for _, ts in repo_files for e, _ in enum_declarations(ts) if e}
        classes = {ts[i + 1].v for _, ts in repo_files for i, t in enumerate(ts[:-1]) if t.v == "class_name"}
        for f, ts in repo_files:
            casts = []
            enum_uses(ts, enums, classes, c, casts)
            freeing_at["cast to an enum with as"] += [f"{name}/{f}:{cast}" for cast in casts]
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
    print(f"lambdas named: {c['lambda_named']}; with default values: {c['lambda_default']}; in methods: {c['lambda_in_method']}, "
          f"assigning a local of the method: {c['lambda_capture_assign']}, through its members or subscripts: {c['lambda_capture_member']}")
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
    games = {name for name, _, _, group in G.REPOS if group == "game"}
    tool_games = ", ".join(f"{r} {n['tool']}" for r, n in sorted(by_repo.items(), key=lambda x: -x[1]["tool"]) if n["tool"] and r in games)
    tool_total = sum(n["tool"] for n in by_repo.values())
    groups = ", ".join(f"{g} {c['tool_' + g]}" for g in sorted({group for _, _, _, group in G.REPOS}) if c["tool_" + g])
    print(f"files with @tool: {tool_total} ({groups}); in games: {tool_games}; naming an editor plugin class: {c['tool_editor_api']}")
    strings = ", ".join(f"{r} {n['multiline_string']}" for r, n in sorted(by_repo.items(), key=lambda x: -x[1]["multiline_string"]) if n["multiline_string"])
    print(f"strings in single or double quotes with a line break: {sum(n['multiline_string'] for n in by_repo.values())} ({strings})")
    groups = ", ".join(f"{g} {c['preload_gd_' + g]}" for g in sorted({group for _, _, _, group in G.REPOS}) if c["preload_gd_" + g])
    preload_repos = ", ".join(f"{r} {n['preload_gd']}" for r, n in sorted(by_repo.items(), key=lambda x: -x[1]["preload_gd"])[:3])
    print(f"preloads of .gd files: {c['preload_gd']} ({groups}; most in {preload_repos}); "
          f"into constants {c['preload_gd_const']}, .new() {c['preload_gd_new']}, other members {c['preload_gd_member']}, "
          f"values {c['preload_gd_value']}")
    print(f"uses of those constants: new() {c['alias_new']} in {len(files['new'])} files, "
          f"other calls {c['alias_static_call']} in {len(files['static_call'])} files, "
          f"other members {c['alias_member']} in {len(files['member'])} files, is {c['alias_is']}, "
          f"types {c['alias_type']}, values {c['alias_value']}")
    print(f"enums: {c['enum']} ({c['enum_named']} named, {c['enum_unnamed']} unnamed), without the value 0: {c['enum_without_zero']}")
    print(f"casts to an enum with as: {c['enum_as']}; @warning_ignore of int_as_enum_without_cast: {c['enum_ignore']}")
    uses = ", ".join(f"{m} {c['enum_dict_' + m]}" for m in sorted(ENUM_DICT_METHODS | {"index", "for"}) if c["enum_dict_" + m])
    print(f"enum names used as a Dictionary: {c['enum_dict']} ({uses}); constants aliasing an enum: {c['enum_alias']}")
    print(f"match over the engine's enums without _: {c['engine_enum_match']}")
    for label, places in freeing_at.items():
        print(f"{label} at: {', '.join(places)}")


if __name__ == "__main__":
    main()
