import subprocess
import sys

README = ("README.zh.md", "README.md")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout.decode("utf-8")


def main():
    staged = set(git("diff", "--cached", "--name-only", "-z").split("\0")) - {""}
    errors = []
    for path in sorted(staged & set(README)):
        other = README[1 - README.index(path)]
        if other not in staged:
            errors.append(f"{path} is staged without {other}; change both versions in the same commit.")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
