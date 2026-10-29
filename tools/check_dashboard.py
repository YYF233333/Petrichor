"""Check the tiny project dashboard; no third-party dependencies.

Usage: python tools/check_dashboard.py [--staged]
Format: optional first line '# Dashboard', then blank lines or entries:
- 2026-10-29T12:00:00Z Verify one current fact

All text counts toward 600 Unicode code points (CRLF counts as one LF),
including the title, spaces, timestamps and trailing newline. At most six
entries; each expires at exactly 72 hours. Renewal is manual after checking
its contents. The hook checks the index; the default checks the working file.
"""

import argparse
from datetime import datetime, timedelta, timezone
from pathlib import Path
import re
import subprocess
import sys

LIMIT = 600
MAX_ENTRIES = 6
LIFETIME = timedelta(hours=72)
ENTRY = re.compile(r"- ([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z) (.+)")
ROOT = Path(__file__).resolve().parent.parent


def check(text, now=None):
    """Return every violation; now is injectable for boundary verification."""
    now = now if now is not None else datetime.now(timezone.utc)
    text = text.replace("\r\n", "\n")
    errors = []
    if len(text) > LIMIT:
        errors.append(f"{len(text)} characters; maximum is {LIMIT} (Unicode code points).")
    if "\r" in text:
        errors.append("Bare CR is invalid; use LF or CRLF line endings.")
    entries = 0
    for number, line in enumerate(text.split("\n"), 1):
        if not line.strip() or (number == 1 and line == "# Dashboard"):
            continue
        entries += 1
        match = ENTRY.fullmatch(line)
        if not match or not match[2].strip():
            errors.append(f"line {number}: expected '- YYYY-MM-DDTHH:MM:SSZ text'.")
            continue
        stamp, body = match.groups()
        try:
            created = datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        except ValueError:
            errors.append(f"line {number}: invalid UTC timestamp {stamp}: {body}")
            continue
        if created > now:
            errors.append(f"line {number}: future timestamp {stamp}: {body}")
        elif now - created >= LIFETIME:
            errors.append(f"line {number}: expired (72h): {stamp}: {body}; remove it or verify and renew its timestamp.")
    if entries > MAX_ENTRIES:
        errors.append(f"{entries} entries; maximum is {MAX_ENTRIES}.")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staged", action="store_true", help="Check DASHBOARD.md from the Git index, as the hook does.")
    args = parser.parse_args()
    try:
        if args.staged:
            result = subprocess.run(["git", "-C", str(ROOT), "show", ":DASHBOARD.md"], capture_output=True)
            if result.returncode:
                print("DASHBOARD.md: missing or unreadable in the index. Include the dashboard in the intended commit; working-tree dates cannot replace staged content.", file=sys.stderr)
                return 1
            data = result.stdout
        else:
            data = (ROOT / "DASHBOARD.md").read_bytes()
        errors = check(data.decode("utf-8"))
    except (OSError, UnicodeError) as error:
        print(f"DASHBOARD.md: cannot read UTF-8 content: {error}", file=sys.stderr)
        return 1
    for error in errors:
        print(f"DASHBOARD.md: {error}", file=sys.stderr)
    if errors:
        return 1
    print("DASHBOARD.md: OK" + (" (index)" if args.staged else " (working tree)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
