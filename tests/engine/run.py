"""Engine tests: run the test project headless with a Godot 4.7 editor binary.

Usage:
    scons target=editor && scons target=template_debug
    python tests/engine/run.py <godot editor binary>    (or set GODOT)

Checks the game run, the editor side (placeholders, saving, the undo probe), the
other step-2 probes of the architecture, and an exported pack run in a
directory without sources.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE / "project"

GAME_OUTPUT = [
    "check: message = from the scene",
    "check: Hello, from the scene",
    "check: Hello, set from GDScript",
    'check: has_method("greet") = true',
    "greeter: _ready, message = set from GDScript",
]

failures = []


def run(godot, *args):
    result = subprocess.run(
        [godot, "--headless", *args], capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    return result.returncode, result.stdout + result.stderr


def expect(name, ok, output=""):
    print(("ok      " if ok else "FAILED  ") + name)
    if not ok:
        failures.append(name)
        if output:
            print(output)


def expect_run(name, code, output, lines=()):
    """The run exits with 0, reports no errors, and prints `lines` in order."""
    errors = [line for line in output.splitlines() if "ERROR" in line]
    remaining = iter(line.strip() for line in output.splitlines())
    in_order = all(any(out == line for out in remaining) for line in lines)
    expect(name, code == 0 and not errors and in_order, output)


def main():
    godot = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GODOT")
    if not godot:
        sys.exit(__doc__)

    run(godot, "--path", str(PROJECT), "--import")

    # The game reads the bytecode the editor writes (architecture §2.4).
    code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--petrichor-build")
    expect_run("build", code, output)
    expect("build: every script compiles", ", 0 with errors" in output, output)

    code, output = run(godot, "--path", str(PROJECT))
    expect_run("game run", code, output, GAME_OUTPUT)

    code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--editor-checks")
    expect_run("editor checks", code, output, ["editor check: 0 failure(s)"])

    code, output = run(godot, "--path", str(PROJECT), "res://probes/probes.tscn")
    expect_run("probes", code, output, ["probe: 0 failure(s)"])

    code, output = run(godot, "--path", str(PROJECT), "res://language/language.tscn")
    expect_run("language", code, output, ["language: 0 failure(s)"])

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pack = tmp / "game.pck"
        code, output = run(godot, "--path", str(PROJECT), "--export-pack", "Windows Desktop", str(pack))
        expect_run("export", code, output)

        # A project directory with the extension but without sources.
        runner = tmp / "runner"
        (runner / ".godot").mkdir(parents=True)
        shutil.copy(PROJECT / "project.godot", runner)
        shutil.copy(PROJECT / "petrichor.gdextension", runner)
        shutil.copy(PROJECT / ".godot" / "extension_list.cfg", runner / ".godot")
        shutil.copytree(PROJECT / "bin", runner / "bin")
        code, output = run(godot, "--path", str(runner), "--script", str(HERE / "export_check.gd"), "--", str(pack))
        expect_run("exported pack run", code, output, ["export check: greeter.petc header = PETC", *GAME_OUTPUT])

    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
