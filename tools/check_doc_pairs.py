import subprocess
import sys

import check_spec_translation

PAIRS = (
    ("README.zh.md", "README.md"),
    ("docs/getting-started.zh.md", "docs/getting-started.md"),
)


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout.decode("utf-8")


def main():
    staged = set(git("diff", "--cached", "--name-only", "-z").split("\0")) - {""}
    errors = []
    for pair in PAIRS:
        for path in sorted(staged & set(pair)):
            other = pair[1 - pair.index(path)]
            if other not in staged:
                errors.append(f"{path} is staged without {other}; change both versions in the same commit.")
    for error in errors:
        print(error, file=sys.stderr)
    spec_stale = check_spec_translation.SOURCE in staged and check_spec_translation.main()
    return 1 if errors or spec_stale else 0


if __name__ == "__main__":
    sys.exit(main())
