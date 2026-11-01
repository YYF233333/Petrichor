import sys

LIMIT = 72


def main():
    lines = []
    with open(sys.argv[1], encoding="utf-8", errors="replace") as file:
        for line in file.read().split("\n"):
            if line.startswith("# ------------------------ >8"):
                break
            if not line.startswith("#"):
                lines.append(line.rstrip())
    while lines and not lines[0]:
        lines.pop(0)
    if not lines or lines[0].startswith(("Merge ", 'Revert "')):
        return 0
    errors = []
    if len(lines[0]) > LIMIT:
        errors.append(f"the subject has {len(lines[0])} characters; keep it within {LIMIT}.")
    if len(lines) > 1 and lines[1]:
        errors.append("the subject runs on to a second line; leave a blank line before the body.")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
