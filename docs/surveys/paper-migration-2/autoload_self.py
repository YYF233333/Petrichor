"""Count first-party lines that register a node on an autoload member: `Autoload.member = self`.

Usage: python autoload_self.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (the 20 repositories)
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

AUTOLOAD_SECTION = re.compile(r"\[autoload\]\n(.*?)(?:\n\[|\Z)", re.S)
REGISTER = re.compile(r"^\s*(\w+)\.(\w+)\s*=\s*self\b")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    total = collections.Counter()
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        autoloads = set()
        for f in repo.files:
            if f.endswith("project.godot"):
                m = AUTOLOAD_SECTION.search(repo.read(f))
                if m:
                    autoloads.update(ln.split("=")[0].strip() for ln in m.group(1).splitlines() if "=" in ln)
        for f in repo.files:
            if f.endswith(".gd") and repo.first_party(f):
                for i, line in enumerate(repo.read(f).splitlines(), 1):
                    m = REGISTER.match(line)
                    if m and m.group(1) in autoloads:
                        total[name] += 1
                        print(f"{name}/{f}:{i}  {line.strip()}")
    print(f"total {sum(total.values())}: " + ", ".join(f"{k} {v}" for k, v in total.most_common()))


if __name__ == "__main__":
    main()
