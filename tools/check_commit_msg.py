import sys

LIMIT = 72
AREAS = (
    "Spec",
    "Arch",
    "Survey",
    "Compiler",
    "Bytecode",
    "Runtime",
    "Adapter",
    "Editor",
    "Tests",
    "Bench",
    "Tools",
    "Docs",
    "Backlog",
)


def check(lines):
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    if not lines or lines[0].startswith('Revert "'):
        return []

    errors = []
    subject = lines[0]
    if len(subject) > LIMIT:
        errors.append(f"the subject has {len(subject)} characters; keep it within {LIMIT}.")
    area, sep, summary = subject.partition(": ")
    if not sep or area not in AREAS:
        errors.append(f"start the subject with '<Area>: ', where Area is one of {', '.join(AREAS)}.")
    elif not summary or not summary[0].isupper():
        errors.append("capitalize the summary after '<Area>: '.")
    if subject.endswith("."):
        errors.append("leave off the full stop at the end of the subject.")

    if len(lines) > 1 and lines[1]:
        errors.append("the subject runs on to a second line; leave a blank line before the body.")
    for number, line in enumerate(lines[2:], 3):
        if len(line) > LIMIT and "http://" not in line and "https://" not in line and " " in line.lstrip(" -*"):
            errors.append(f"line {number} has {len(line)} characters; wrap the body at {LIMIT} columns.")
    return errors


def main():
    lines = []
    with open(sys.argv[1], encoding="utf-8", errors="replace") as file:
        for line in file.read().split("\n"):
            if line.startswith("# ------------------------ >8"):
                break
            if not line.startswith("#"):
                lines.append(line.rstrip())
    errors = check(lines)
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
