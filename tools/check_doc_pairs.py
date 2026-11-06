import re
import subprocess
import sys

SPEC = ("docs/language-spec.zh.md", "docs/language-spec.en.md")
MARK = re.compile(r"(#+ |```)")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout.decode("utf-8")


PAIRS = (SPEC, ("README.zh.md", "README.md"))


def counterpart(path):
    for pair in PAIRS:
        if path in pair:
            return pair[1 - pair.index(path)]
    return None


def shape(line):
    if not line.strip():
        return "blank"
    match = MARK.match(line)
    return match[1] if match else "text"


def main():
    staged = set(git("diff", "--cached", "--name-only", "-z").split("\0")) - {""}
    errors = []
    for path in sorted(staged):
        other = counterpart(path)
        if other and other not in staged:
            errors.append(f"{path} is staged without {other}; change both versions in the same commit.")
    if staged & set(SPEC):
        zh, en = (git("show", f":{path}").split("\n") for path in SPEC)
        if len(zh) != len(en):
            errors.append(f"{SPEC[0]} has {len(zh)} lines, {SPEC[1]} {len(en)}; keep them line-aligned.")
        else:
            for number, (a, b) in enumerate(zip(zh, en), 1):
                if shape(a) != shape(b):
                    errors.append(f"line {number} of the spec is {shape(a)} in Chinese but {shape(b)} in English; keep them line-aligned.")
                    break
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
