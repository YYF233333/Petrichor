"""Benchmarks against GDScript (architecture §4).

Usage:
    scons target=editor
    python benchmarks/run.py <Godot 4.7 editor binary>

Each case in project/cases/ is written in GDScript and in this language with the
same algorithm. The comparison needs an optimized engine, such as an official
release, not a dev_build: GDScript runs inside the engine binary.
"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE.parent / "tests" / "engine" / "project"


def run(godot, *args):
    result = subprocess.run([godot, "--headless", *args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    return result.returncode, result.stdout + result.stderr


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    godot = sys.argv[1]
    _, version = run(godot, "--version")
    print(f"engine: {version.strip().splitlines()[-1]}")

    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp) / "bench"
        shutil.copytree(HERE / "project", project)
        shutil.copytree(TESTS / "bin", project / "bin")
        shutil.copy(TESTS / "petrichor.gdextension", project)
        # Not checked, as import_project() of tests/engine/run.py explains.
        run(godot, "--path", str(project), "--import")
        code, output = run(godot, "-e", "--path", str(project), "--", "--petrichor-build")
        if code != 0 or ", 0 with errors" not in output:
            sys.exit(output)
        code, output = run(godot, "--path", str(project))

    rows = [line.split()[1:] for line in output.splitlines() if line.startswith("bench: ")]
    if code != 0 or not rows:
        sys.exit(output)
    print(f"{'case':<12}{'GDScript':>12}{'Petrichor':>12}{'speedup':>10}")
    for case, gdscript, petrichor, same in rows:
        speedup = int(gdscript) / max(int(petrichor), 1)
        note = "" if same == "true" else "  results differ"
        print(f"{case:<12}{int(gdscript) / 1000:>10.1f}ms{int(petrichor) / 1000:>10.1f}ms{speedup:>9.2f}x{note}")

    # Times are evidence only when every case ran once, both versions computed
    # the same result, and nothing reported an error.
    problems = [line for line in output.splitlines() if re.search(r"\bERROR\b", line)]
    expected = sorted(path.stem for path in (HERE / "project" / "cases").glob("*.pet"))
    measured = sorted(row[0] for row in rows)
    if measured != expected:
        problems.append(f"cases measured: {measured}; expected each of {expected} once")
    problems += [f"{row[0]}: results differ" for row in rows if row[3] != "true"]
    if problems:
        sys.exit("\n".join(problems))


if __name__ == "__main__":
    main()
