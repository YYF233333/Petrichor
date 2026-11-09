"""The constants and utility functions GDScript defines itself (@GDScript), which the earlier counts missed: how
many first-party GDScript files of the corpus use each.

Usage: python gdscript_builtins.py CORPUS_DIR [--samples N]
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    --samples N: also print the first N locations of each name, for reading

The names are those of modules/gdscript/gdscript_tokenizer.cpp:540-546 and
modules/gdscript/gdscript_utility_functions.cpp:572-590 (ed1daf0), less load(), preload() and print_debug(), which
the implementation supports. A constant counts where its name is not after `.`; a function where its name is
followed by `(` and is neither after `.` nor declared by the file with `func`.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

CONSTANTS = ["PI", "TAU", "INF", "NAN"]
FUNCTIONS = ["range", "len", "char", "ord", "Color8", "type_exists", "print_stack", "get_stack", "convert",
             "is_instance_of", "inst_to_dict", "dict_to_inst"]
ANY = "any of them"


def main():
    args = sys.argv[1:]
    samples = 0
    if "--samples" in args:
        k = args.index("--samples")
        samples = int(args[k + 1])
        del args[k:k + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    corpus = args[0]

    stats = {key: {"files": 0, "lines": 0, "uses": collections.Counter(), "hit": collections.Counter()}
             for key in ("all", "games")}
    per_repo = []
    where = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(corpus, name, group)
        repo_files = repo_hit = 0
        for f in sorted(f for f in repo.files if f.endswith(".gd") and repo.first_party(f)):
            src = repo.read(f)
            ts = [t for t in G.tokenize(src) if t.k != "NL"]
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            declared = {ts[i + 1].v for i in range(len(ts) - 1) if ts[i].v == "func"}
            used = collections.Counter()
            for i, t in enumerate(ts):
                if t.k != "ID" or (i and ts[i - 1].v == "."):
                    continue
                called = i + 1 < len(ts) and ts[i + 1].v == "("
                if t.v in CONSTANTS or (t.v in FUNCTIONS and called and t.v not in declared):
                    used[t.v] += 1
                    if len(where[t.v]) < samples:
                        where[t.v].append(f"{name}/{f}:{t.line}")
            if used:
                used[ANY] = 1
            repo_files += 1
            repo_hit += bool(used)
            for key in ("all", "games") if group == "game" else ("all",):
                s = stats[key]
                s["files"] += 1
                s["lines"] += lines
                s["uses"].update(used)
                s["hit"].update(used.keys())
        per_repo.append((name, group, repo_files, repo_hit))

    for key, title in (("all", "All repositories"), ("games", "Game repositories")):
        s = stats[key]
        print(f"== {title}: {s['files']} files, {s['lines']} code lines")
        for c in [ANY] + CONSTANTS + FUNCTIONS:
            n = s["hit"][c]
            uses = "" if c == ANY else f"  uses {s['uses'][c]:5d} ({1000 * s['uses'][c] / s['lines']:4.1f} per 1,000 lines)"
            print(f"   {c:16s} files {n:5d} ({100 * n / s['files']:4.1f}%){uses}")
    print("== Per repository: files, files using any of them")
    for name, group, n, hit in per_repo:
        print(f"   {name:28s} {group:7s} {n:5d} {hit:5d} ({100 * hit / n if n else 0:4.1f}%)")
    if samples:
        print("== Samples")
        for c in CONSTANTS + FUNCTIONS:
            print(f"   {c}")
            for loc in where[c]:
                print(f"      {loc}")


if __name__ == "__main__":
    main()
