"""Fails when a string literal in the sources cites a section of a document,
as "(spec §5.7)": the spec does not ship, so a diagnostic gives its own
reason (spec §8.4)."""

import sys


def literals(text):
    """The line and contents of each string literal, outside comments."""
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                return
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        c = text[i]
        if c in "'\"":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            if c == '"':
                yield text.count("\n", 0, i) + 1, text[i + 1 : j]
            i = j + 1
            continue
        i += 1


def main():
    errors = []
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as file:
            for line, literal in literals(file.read()):
                if "§" in literal:
                    errors.append(f"{path}:{line}: a string cites a section; let the message give the reason instead.")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
