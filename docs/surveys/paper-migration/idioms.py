"""Count, over first-party corpus code, the GDScript idioms the paper migration found costly.

Usage: python idioms.py CORPUS_DIR [NAME=DIR ...]

CORPUS_DIR holds the 14 repositories of the other surveys, one per subdirectory; only
first-party code counts (scope.py). Each NAME=DIR adds a directory counted in full, e.g.
Ring's breakout/scenes. Lines are logical lines of code after clean_lines (comments
removed, string contents blanked, continuation lines joined). Rates are per 1,000 lines.
"""
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "property-accessors"))
from extract import clean_lines, logical_lines  # noqa: E402
from scope import scope_of  # noqa: E402

VARIANT_MATH = "abs|ceil|clamp|floor|lerp|max|min|round|sign|snapped|wrap"
NODE_PATH = r"(?<![\w)\]\"'])[$%](?:\"[^\"]*\"|'[^']*'|[\w/.]*\w(?![\w/]))"
PATTERNS = {
    # int or float in a type position: annotation, return type, typed container, as-cast.
    "int-float-type": re.compile(r"(?::|->)\s*(?:int|float)\b(?!\s*\()|\[\s*(?:int|float)\s*[\],]"
                                 r"|,\s*(?:int|float)\s*\]|\bas\s+(?:int|float)\b"),
    # Global math utilities whose engine signature returns Variant (not method calls).
    "variant-math": re.compile(rf"(?<![\w.])(?:{VARIANT_MATH})\s*\("),
    "get_tree": re.compile(r"\bget_tree\s*\(\s*\)"),
    "tween-timer": re.compile(r"\b(?:create_tween|create_timer)\s*\("),
    "node-as": re.compile(NODE_PATH + r"\s+as\s+[A-Z]\w*"),
    "onready-untyped": re.compile(r"@onready\s+var\s+\w+\s*:?=\s*" + NODE_PATH + r"(?!\s+as\b)"),
    "node-member": re.compile(NODE_PATH + r"\.\w"),
    "has_method": re.compile(r"\bhas_method\s*\("),
    "instantiate": re.compile(r"\binstantiate\s*\("),
}
FUNC_DEF = re.compile(rf"\bfunc\s+(?:{VARIANT_MATH})\s*\(")
IS_TEST = re.compile(r"(?<![\w.])([A-Za-z_]\w*)\s+is\s+[A-Z]\w*")
FOR_INT = re.compile(r"^for\s+\w+(?:\s*:\s*\w+)?\s+in\s+(?:\d+|[\w.]+\.size\(\)|len\([^()]*\))\s*:")
KEYS = list(PATTERNS) + ["is-then-member", "for-in-int"]
GAMES = {"godot-demo-projects", "godot-open-rpg", "tps-demo", "Starter-Kit-3D-Platformer",
         "Starter-Kit-City-Builder", "Starter-Kit-FPS", "Starter-Kit-Racing"}


def is_then_member(lines, idx):
    """`x is T` followed by x.member later in the same condition, or in the body of an if/elif."""
    indent, text = lines[idx]
    hits = 0
    for m in IS_TEST.finditer(text):
        name = m.group(1)
        if name in ("not", "and", "or", "if", "elif", "return"):
            continue
        member = re.compile(rf"(?<![\w.]){re.escape(name)}\.\w")
        if member.search(text[m.end():]):
            hits += 1
            continue
        if not re.match(r"(?:if|elif)\b", text):
            continue
        j = idx + 1
        while j < len(lines) and lines[j][0] > indent:
            if member.search(lines[j][1]):
                hits += 1
                break
            j += 1
    return hits


def scan(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read().replace("\r\n", "\n")
    code, starts = clean_lines(src)
    lines = [(ind, text) for _, _, ind, text in logical_lines(src.split("\n"), code, starts)]
    c = dict.fromkeys(KEYS, 0)
    for i, (_, t) in enumerate(lines):
        for k, pat in PATTERNS.items():
            c[k] += len(pat.findall(t))
        c["variant-math"] -= len(FUNC_DEF.findall(t))
        c["is-then-member"] += is_then_member(lines, i)
        c["for-in-int"] += 1 if FOR_INT.match(t) else 0
    return len(lines), c


def collect(name, root, use_scope):
    total, loc = dict.fromkeys(KEYS, 0), 0
    for dp, dn, fn in os.walk(root):
        dn[:] = sorted(d for d in dn if d != ".git")
        for f in sorted(fn):
            if not f.endswith(".gd"):
                continue
            p = os.path.join(dp, f)
            if use_scope and scope_of(name, os.path.relpath(p, root).replace("\\", "/")) != "first-party":
                continue
            n, c = scan(p)
            loc += n
            for k in KEYS:
                total[k] += c[k]
    return name, loc, total


def main():
    corpus = sys.argv[1]
    repos = [collect(r, os.path.join(corpus, r), True)
             for r in sorted(os.listdir(corpus)) if os.path.isdir(os.path.join(corpus, r))]
    extra = [collect(*arg.split("=", 1), False) for arg in sys.argv[2:]]

    head = "| | lines | " + " | ".join(KEYS) + " |\n|---|" + "---:|" * (len(KEYS) + 1)
    print("## counts\n" + head)
    for name, loc, c in repos + extra:
        print(f"| {name} | {loc} | " + " | ".join(str(c[k]) for k in KEYS) + " |")

    def rate(label, group):
        loc = sum(g[1] for g in group)
        vals = [sum(g[2][k] for g in group) * 1000 / loc for k in KEYS]
        print(f"| {label} | {loc} | " + " | ".join(f"{v:.1f}" for v in vals) + " |")

    print("\n## per 1,000 lines\n" + head)
    rate("corpus", repos)
    rate("games and demos", [g for g in repos if g[0] in GAMES])
    rate("plugins and tools", [g for g in repos if g[0] not in GAMES])
    for g in extra:
        rate(g[0], [g])
    big = [g for g in repos if g[1] >= 1000]
    medians = []
    for k in KEYS:
        v = sorted(g[2][k] * 1000 / g[1] for g in big)
        medians.append(v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2)
    print(f"| median of {len(big)} repos >= 1000 lines | - | " + " | ".join(f"{m:.1f}" for m in medians) + " |")


if __name__ == "__main__":
    main()
