import re
import subprocess
import sys

ZH_LIMIT, EN_LIMIT = 40, 20
CJK = re.compile(r"[㐀-鿿豈-﫿]")
WORD = re.compile(r"[A-Za-z0-9_]+(?:['.\-][A-Za-z0-9_]+)*")
ZH_END = re.compile(r"(?<=[。！？])")
EN_END = re.compile(r"(?<=[.!?])\s+")
ABBREVIATIONS = re.compile(r"\b(e\.g|i\.e|etc|vs|cf)\.")
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout.decode("utf-8")


def added_lines(path):
    lines = set()
    for line in git("diff", "--cached", "-U0", "--no-color", "-M", "--", path).splitlines():
        match = HUNK.match(line)
        if match:
            start, count = int(match[1]), int(match[2] or 1)
            lines.update(range(start, start + count))
    return lines


def plain(text):
    text = re.sub(r"`[^`]*`", "C", text)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"https?://\S+", "U", text)
    text = re.sub(r"^\s*(?:[-*>]|\d+\.)\s+", "", text)
    return text.replace("**", "")


def too_long(sentence):
    if CJK.search(sentence):
        size = len(CJK.findall(sentence)) + len(WORD.findall(CJK.sub(" ", sentence)))
        return size > ZH_LIMIT and f"{size} 字 (limit {ZH_LIMIT})"
    size = len(WORD.findall(sentence))
    return size > EN_LIMIT and f"{size} words (limit {EN_LIMIT})"


def sentences(line):
    cells = line.strip().strip("|").split("|") if line.lstrip().startswith("|") else [line]
    for cell in cells:
        text = ABBREVIATIONS.sub(lambda m: m[1].replace(".", ""), plain(cell))
        for part in ZH_END.split(text):
            yield from EN_END.split(part)


def check(path, everything):
    text = git("show", f":{path}") if not everything else open(path, encoding="utf-8").read()
    wanted = None if everything else added_lines(path)
    errors, fence = [], False
    for number, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("```"):
            fence = not fence
            continue
        if fence or line.startswith("#") or re.fullmatch(r"\s*\|[-:| ]+\|\s*", line):
            continue
        if wanted is not None and number not in wanted:
            continue
        for sentence in sentences(line):
            reason = too_long(sentence)
            if reason:
                errors.append(f"{path}:{number}: sentence of {reason}: {sentence.strip()[:40]}…")
    return errors


def main():
    everything = "--all" in sys.argv
    paths = [arg for arg in sys.argv[1:] if arg != "--all"]
    errors = [error for path in paths for error in check(path, everything)]
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"{len(errors)} sentences are too long; split them or turn them into lists (AGENTS.md, Documentation).", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
