"""Layout habits of the corpus, as evidence for the formatter's rules (specification §13.2).

Usage: python layout.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Counts, in first-party .gd files (a tab is 4 columns, the editor's default indent size):
  - files indented with tabs, with spaces, with both, and without indentation;
  - continuation lines inside brackets, by the indentation added to the line of the innermost open bracket, in
    parentheses and in square brackets or braces; closing brackets that begin a line, at that line's indentation
    or not;
  - dictionary literals on one line, with or without a space inside their braces;
  - comments after code, by the number of spaces before them;
  - blank lines before a function or class at the top level, and before a function in an inner class that is not
    its first member, with annotations and comments directly above counted with it; runs of blank lines inside
    indented code;
  - code lines (not blank, not only a comment) longer than 100 columns;
  - lines ending with `\\`, blocks written on their header's line (`if x: return`) and `;` between statements.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

OPEN, CLOSE = "([{", ")]}"


def width(ws):
    w = 0
    for ch in ws:
        w = (w // 4 + 1) * 4 if ch == "\t" else w + 1
    return w


def indent(line):
    return width(re.match(r"[ \t]*", line).group())


def code_only(src):
    """The text with strings and comments replaced by spaces, so that offsets and lines stay."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "#":
            j = src.find("\n", i)
            j = n if j < 0 else j
        elif c in "\"'":
            triple = src.startswith(c * 3, i)
            j = i + (3 if triple else 1)
            while j < n:
                if src[j] == "\\":
                    j += 2
                elif triple and src.startswith(c * 3, j):
                    j += 3
                    break
                elif not triple and src[j] in (c, "\n"):
                    j += 1
                    break
                else:
                    j += 1
        else:
            i += 1
            continue
        for k in range(i, min(j, n)):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def blanks_above(lines, idx, attached):
    """Blank lines above line `idx`, past the annotation and comment lines directly above it."""
    j = idx - 1
    while j >= 0 and lines[j].strip() and attached(lines[j]):
        j -= 1
    blanks = 0
    while j >= 0 and not lines[j].strip():
        blanks += 1
        j -= 1
    return j, blanks


def count(R, rel, C):
    src = R.read(rel).replace("\r\n", "\n")
    lines = src.split("\n")
    clines = code_only(src).split("\n")
    C["files"]["all"] += 1
    tabs = any(ln.startswith("\t") for ln in lines)
    spaces = any(ln.startswith(" ") and ln.strip() for ln in lines)
    C["files"]["both" if tabs and spaces else "tabs" if tabs else "spaces" if spaces else "unindented"] += 1

    for idx, ln in enumerate(lines):
        if re.match(r"(static\s+)?func\b|class\s+\w+.*:\s*$", ln):
            j, blanks = blanks_above(lines, idx, lambda s: s[0] in "@#")
            if j >= 0:
                C["blank_before_top"][min(blanks, 3)] += 1
        elif re.match(r"[ \t]+(static\s+)?func\b", ln):
            j, blanks = blanks_above(lines, idx, lambda s: s.lstrip()[0] in "@#")
            if j >= 0 and not re.match(r"\s*class\b", lines[j]):
                C["blank_before_inner"][min(blanks, 2)] += 1
    run = 0
    for idx, ln in enumerate(lines):
        if not ln.strip():
            run += 1
            continue
        if run and ln[0] in "\t " and lines[idx - run - 1][:1] in ("\t", " "):
            C["blank_runs_in_code"][min(run, 2)] += 1
        run = 0

    stack = []  # (line of an open bracket, the bracket)
    for li, cl in enumerate(clines):
        stripped = cl.strip()
        if stripped:
            C["code_lines"]["all"] += 1
            if indent(lines[li]) + len(lines[li].strip(" \t")) > 100:
                C["code_lines"][">100"] += 1
        if stack and stripped:
            d = indent(lines[li]) - indent(lines[stack[-1][0]])
            if stripped[0] in CLOSE:
                C["closing_line"]["at_opener" if d == 0 else "other"] += 1
            else:
                kind = "cont_paren" if stack[-1][1] == "(" else "cont_list"
                C[kind]["+1" if d == 4 else "+2" if d == 8 else "other"] += 1
        for ch in cl:
            if ch in OPEN:
                stack.append((li, ch))
            elif ch in CLOSE and stack:
                stack.pop()
        if cl.rstrip().endswith("\\"):
            C["misc"]["backslash"] += 1
        if not stack and re.match(r"\s*(if|elif|else|for|while)\b[^:]*:\s*\S", cl) and ":=" not in cl:
            C["misc"]["one_line_block"] += 1
        if re.search(r";\s*\S", cl):
            C["misc"]["semicolon"] += 1
        for m in re.finditer(r"\{([^{}\n]*:[^{}\n]*)\}", cl):
            inner = lines[li][m.start() + 1:m.end() - 1]
            if inner.strip():
                C["dict_one_line"]["spaced" if inner[0] == " " and inner[-1] == " " else "tight" if inner[0] != " " and inner[-1] != " " else "mixed"] += 1
        rest = lines[li][len(cl.rstrip()):]
        if stripped and rest.lstrip(" ").startswith("#"):
            C["comment_gap"][min(len(rest) - len(rest.lstrip(" ")), 3)] += 1


def main():
    totals = collections.defaultdict(collections.Counter)
    per_repo = {}
    for name, _url, _sha, group in G.REPOS:
        R = G.Repo(sys.argv[1], name, group)
        C = collections.defaultdict(collections.Counter)
        for rel in R.files:
            if rel.endswith(".gd") and R.first_party(rel):
                count(R, rel, C)
        per_repo[name] = C
        for k, v in C.items():
            totals[k].update(v)
    for k in sorted(totals):
        print(f"{k}: {dict(sorted(totals[k].items(), key=lambda x: str(x[0])))}")
    print()
    print("blank lines before top-level functions and classes, by repository (1 / 2):")
    for name, C in per_repo.items():
        print(f"  {name}: {C['blank_before_top'][1]} / {C['blank_before_top'][2]}")
    print("code lines longer than 100 columns, by repository:")
    for name, C in sorted(per_repo.items(), key=lambda x: -x[1]["code_lines"][">100"]):
        if C["code_lines"][">100"]:
            print(f"  {name}: {C['code_lines']['>100']}")


main()
