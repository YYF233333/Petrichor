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
import threading
import time
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


TIMEOUT = 120
PROMPT = b"debug> "


def run(godot, *args):
    # A script error can keep a scene from quitting; the timeout ends it.
    try:
        result = subprocess.run(
            [godot, "--headless", *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=TIMEOUT,
        )
    except subprocess.TimeoutExpired as timeout:
        output = (timeout.stdout or "") + (timeout.stderr or "")
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        return -1, output + "\n(timed out)"
    return result.returncode, result.stdout + result.stderr


def run_debugger(godot, *args, commands):
    """Runs under the engine's local debugger (`-d`), sending one command per prompt.

    The local debugger reads a whole pipe buffer as one command line on Windows
    (os_windows.cpp:2094-2107), and repeats its prompt forever once standard input
    is closed (local_debugger.cpp:148), so each command waits for its prompt and
    standard input stays open."""
    process = subprocess.Popen(
        [godot, "--headless", *args],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = bytearray()
    changed = threading.Condition()

    def read():
        while chunk := process.stdout.read1(4096):
            with changed:
                output.extend(chunk)
                changed.notify()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    deadline = time.monotonic() + TIMEOUT
    timed_out = False
    for sent, command in enumerate(commands):
        with changed:
            prompted = changed.wait_for(
                lambda: output.count(PROMPT) > sent or process.poll() is not None,
                timeout=max(0, deadline - time.monotonic()),
            )
        if not prompted or process.poll() is not None:
            timed_out = not prompted
            break
        try:
            process.stdin.write(command.encode() + b"\n")
            process.stdin.flush()
        except OSError:
            break
    try:
        code = process.wait(timeout=max(0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        process.kill()
        code = process.wait()
        timed_out = True
    reader.join()
    text = output.decode("utf-8", "replace")
    return (-1, text + "\n(timed out)") if timed_out else (code, text)


def expect(name, ok, output=""):
    print(("ok      " if ok else "FAILED  ") + name, flush=True)
    if not ok:
        failures.append(name)
        if output:
            print(output)


def expect_run(name, code, output, lines=()):
    """The run exits with 0, reports no errors, and prints `lines` in order.

    A line may follow a prompt, such as the debugger's "debug> "."""
    errors = [line for line in output.splitlines() if "ERROR" in line]
    remaining = iter(line.strip() for line in output.splitlines())
    in_order = all(any(out.endswith(line) for out in remaining) for line in lines)
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

    # Spec §12.5: breakpoints, locals, members, stepping and a break on a
    # program error, through the engine's local debugger driven on standard input.
    code, output = run_debugger(
        godot,
        "-d",
        "--breakpoints",
        "res://language/breakpoint.pet:8",
        "--path",
        str(PROJECT),
        "res://language/breakpoint.tscn",
        commands=["locals", "members", "s", "members", "c", "c"],
    )
    expect_run(
        "breakpoint",
        code,
        output,
        [
            "Debugger Break, Reason: 'Breakpoint'",
            "*Frame 0 - res://language/breakpoint.pet:8 in function 'step'",
            "n: 20",
            "doubled: 40",
            "calls: 0",
            "*Frame 0 - res://language/breakpoint.pet:9 in function 'step'",
            "calls: 1",
            "breakpoint: step returned 41",
            "Debugger Break, Reason: 'Division by zero.'",
            "*Frame 0 - res://language/breakpoint.pet:12 in function 'divide'",
            "breakpoint: divide returned 0",
        ],
    )

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
