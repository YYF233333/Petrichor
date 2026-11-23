import re
import subprocess
import sys

SOURCE, TRANSLATION = "docs/language-spec.zh.md", "docs/language-spec.md"
RECORD = re.compile(r"<!-- Generated from language-spec\.zh\.md, git blob ([0-9a-f]{40})\.")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout.decode("utf-8")


def main():
    # From the index, which is what the commit holds.
    record = RECORD.search(git("show", f":{TRANSLATION}"))
    current = git("rev-parse", f":{SOURCE}").strip()
    if not record:
        error = f"{TRANSLATION} records no blob of {SOURCE}; regenerate it (tools/spec_translation.md)."
    elif record[1] != current:
        error = f"{TRANSLATION} was generated from blob {record[1][:10]} of {SOURCE}, now {current[:10]}; regenerate it (tools/spec_translation.md)."
    else:
        return 0
    print(error, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
