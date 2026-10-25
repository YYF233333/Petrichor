"""Engine tests: run the test project headless with a Godot 4.7 editor binary.

Usage:
    scons target=editor && scons target=template_debug
    python tests/engine/run.py <godot editor binary> [group...]    (or set GODOT)

Checks the game run, the editor side (placeholders, saving, the undo probe), the
other step-2 probes of the architecture, the language, the debugger, hot reload,
an exported pack run in a directory without sources, and the FPS starter kit
with its migrated scripts. Naming groups runs only those, after the build.
"""

import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE / "project"
FPS_UPSTREAM = HERE.parent.parent / "thirdparty" / "starter-kit-fps"
FPS_MIGRATED = HERE / "fps"
HOT_RELOAD = PROJECT / "language" / "hot_reload"
HOT_RELOAD_BYTECODE = PROJECT / ".godot" / "petrichor" / "language" / "hot_reload"
LIVE_SCRIPT = "res://language/hot_reload/live.pet"

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


class Running:
    """A run whose output is read while it goes on, so that the test can answer it."""

    def __init__(self, godot, *args, stdin=False):
        self.process = subprocess.Popen(
            [godot, "--headless", *args],
            stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.output = bytearray()
        self.ended = False
        self.changed = threading.Condition()
        self.deadline = time.monotonic() + TIMEOUT
        self.timed_out = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        while chunk := self.process.stdout.read1(4096):
            with self.changed:
                self.output.extend(chunk)
                self.changed.notify_all()
        with self.changed:
            self.ended = True
            self.changed.notify_all()

    def _remaining(self):
        return max(0, self.deadline - time.monotonic())

    def wait_for(self, text, count=1):
        """Whether `text` is printed `count` times before the output ends."""
        with self.changed:
            found = self.changed.wait_for(
                lambda: self.output.count(text) >= count or self.ended,
                timeout=self._remaining(),
            ) and self.output.count(text) >= count
        if not found and self._remaining() == 0:
            self.timed_out = True
        return found

    def send(self, line):
        try:
            self.process.stdin.write(line.encode() + b"\n")
            self.process.stdin.flush()
            return True
        except OSError:
            return False

    def finish(self):
        try:
            code = self.process.wait(timeout=self._remaining())
        except subprocess.TimeoutExpired:
            self.process.kill()
            code = self.process.wait()
            self.timed_out = True
        self.reader.join()
        text = self.output.decode("utf-8", "replace")
        return (-1, text + "\n(timed out)") if self.timed_out else (code, text)


def run_debugger(godot, *args, commands):
    """Runs under the engine's local debugger (`-d`), sending one command per prompt.

    The local debugger reads a whole pipe buffer as one command line on Windows
    (os_windows.cpp:2094-2107), and repeats its prompt forever once standard input
    is closed (local_debugger.cpp:148), so each command waits for its prompt and
    standard input stays open."""
    running = Running(godot, *args, stdin=True)
    for sent, command in enumerate(commands):
        if not running.wait_for(PROMPT, sent + 1) or not running.send(command):
            break
    return running.finish()


def encode_variant(value):
    """The engine's binary form of a Variant (marshalls.cpp:1362-1900), for the
    types the editor's messages use here."""
    if isinstance(value, str):
        data = value.encode()
        return struct.pack("<II", 4, len(data)) + data + bytes(-len(data) % 4)
    if isinstance(value, int):
        return struct.pack("<Ii", 2, value)
    if isinstance(value, list):
        return struct.pack("<II", 28, len(value)) + b"".join(encode_variant(item) for item in value)
    raise TypeError(value)


def send_editor_message(connection, command, data):
    """A message from the editor's debugger: [command, thread, data]
    (script_editor_debugger.cpp:79-86), prefixed by its length
    (remote_debugger_peer.cpp:101-127). Thread 1 is the main thread."""
    message = encode_variant([command, 1, data])
    connection.sendall(struct.pack("<I", len(message)) + message)


def install_live_version(version):
    """What saving live.pet in the editor leaves on disk: the source and the
    bytecode compiled from it (architecture §2.4)."""
    shutil.copy(HOT_RELOAD / f"v{version}.pet", HOT_RELOAD / "live.pet")
    shutil.copy(HOT_RELOAD_BYTECODE / f"v{version}.petc", HOT_RELOAD_BYTECODE / "live.petc")


def run_hot_reload(godot):
    """Stands in for the editor while hot_reload_test.gd runs connected to it.

    When the scene asks for a version, it is put in place and announced with
    reload_scripts, as the editor does after a save (script_editor_plugin.cpp:
    738-741, 776-801); the game reloads it at its next idle poll
    (remote_debugger.cpp:689-719)."""
    with socket.create_server(("127.0.0.1", 0)) as server:
        server.settimeout(TIMEOUT)
        install_live_version(1)
        try:
            running = Running(
                godot,
                "--remote-debug",
                f"tcp://127.0.0.1:{server.getsockname()[1]}",
                "--path",
                str(PROJECT),
                "res://language/hot_reload/hot_reload.tscn",
            )
            try:
                connection, _ = server.accept()
            except OSError:
                return running.finish()
            with connection:
                threading.Thread(target=drain, args=(connection,), daemon=True).start()
                for version in (2, 3, 4):
                    if not running.wait_for(f"hot reload: waiting for version {version}".encode()):
                        break
                    install_live_version(version)
                    send_editor_message(connection, "reload_scripts", [LIVE_SCRIPT])
                return running.finish()
        finally:
            (HOT_RELOAD / "live.pet").unlink(missing_ok=True)
            (HOT_RELOAD_BYTECODE / "live.petc").unlink(missing_ok=True)


def drain(connection):
    # The game's messages to the editor are not checked here.
    try:
        while connection.recv(65536):
            pass
    except OSError:
        pass


def prepare_fps(project):
    """The FPS starter kit with the extension and fps_check.gd."""
    shutil.copytree(FPS_UPSTREAM, project, ignore=shutil.ignore_patterns(".git"))
    shutil.copytree(PROJECT / "bin", project / "bin")
    shutil.copy(PROJECT / "petrichor.gdextension", project)
    shutil.copy(FPS_MIGRATED / "fps_check.gd", project)

    settings = project / "project.godot"
    text = settings.read_text(encoding="utf-8")
    text = re.sub(r"^\[autoload\]\r?$", '[autoload]\n\nFpsCheck="*res://fps_check.gd"', text, count=1, flags=re.M)
    if "FpsCheck" not in text:
        text += '\n[autoload]\n\nFpsCheck="*res://fps_check.gd"\n'
    settings.write_text(text, encoding="utf-8")


def migrate_fps(project):
    """Puts each migrated script in place of its GDScript original
    (architecture §5 step 4)."""
    replaced = {}
    for script in FPS_MIGRATED.rglob("*.pet"):
        relative = script.relative_to(FPS_MIGRATED)
        original = project / relative.with_suffix(".gd")
        original.unlink()
        original.with_name(original.name + ".uid").unlink(missing_ok=True)
        shutil.copy(script, project / relative)
        replaced["res://" + relative.with_suffix(".gd").as_posix()] = "res://" + relative.as_posix()

    # Scenes and resources name a script by uid and path; the uid belonged to
    # the original, so only the path stays.
    for file in [*project.rglob("*.tscn"), *project.rglob("*.tres"), project / "project.godot"]:
        lines = file.read_text(encoding="utf-8").split("\n")
        for i, line in enumerate(lines):
            for old, new in replaced.items():
                if f'"{old}"' in line or f'"*{old}"' in line:
                    lines[i] = re.sub(r' uid="[^"]*"', "", line).replace(old, new)
        file.write_text("\n".join(lines), encoding="utf-8")


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
    only = set(sys.argv[2:])

    def wanted(group):
        return not only or group in only

    run(godot, "--path", str(PROJECT), "--import")

    # The game reads the bytecode the editor writes (architecture §2.4).
    code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--petrichor-build")
    expect_run("build", code, output)
    expect("build: every script compiles", ", 0 with errors" in output, output)

    if wanted("game"):
        code, output = run(godot, "--path", str(PROJECT))
        expect_run("game run", code, output, GAME_OUTPUT)

    if wanted("editor"):
        code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--editor-checks")
        expect_run("editor checks", code, output, ["editor check: 0 failure(s)"])

    if wanted("probes"):
        code, output = run(godot, "--path", str(PROJECT), "res://probes/probes.tscn")
        expect_run("probes", code, output, ["probe: 0 failure(s)"])

    if wanted("language"):
        code, output = run(godot, "--path", str(PROJECT), "res://language/language.tscn")
        expect_run("language", code, output, ["language: 0 failure(s)"])

    if wanted("debugger"):
        run_debugger_groups(godot)

    if wanted("export"):
        run_export_groups(godot)

    if wanted("fps"):
        run_fps_group(godot)

    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


def run_fps_group(godot):
    # Architecture §5 step 4: the starter kit runs with its migrated scripts.
    # The original reports errors of its own in a headless run (a sound still
    # playing at exit), so the migrated run may only repeat those.
    with tempfile.TemporaryDirectory() as tmp:
        original = Path(tmp) / "original"
        prepare_fps(original)
        run(godot, "--path", str(original), "--import")
        code, output = run(godot, "--path", str(original))
        expect("fps original run", code == 0 and "fps check: 0 failure(s)" in output, output)
        known_errors = {line for line in output.splitlines() if "ERROR" in line}

        migrated = Path(tmp) / "migrated"
        shutil.copytree(original, migrated)
        migrate_fps(migrated)
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-build")
        expect_run("fps build", code, output)
        expect("fps build: every script compiles", ", 0 with errors" in output, output)
        code, output = run(godot, "--path", str(migrated))
        new_errors = [line for line in output.splitlines() if "ERROR" in line and line not in known_errors]
        expect("fps run", code == 0 and not new_errors and "fps check: 0 failure(s)" in output, output)
        for script in sorted(FPS_MIGRATED.rglob("*.pet")):
            expect(f"fps run: {script.stem} runs migrated", f"fps check: {script.stem} runs PetScript" in output, output)


def run_debugger_groups(godot):
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

    # Spec §12.4: a body change replaces the code of running instances; any
    # other change leaves them on the old code and asks for a new run. Each
    # instance is compared with the code it runs, not with the previous version.
    kept_old_code = f"{LIVE_SCRIPT}: more than function bodies changed since some instances were created. They keep their old code; run the scene again to apply the change."
    code, output = run_hot_reload(godot)
    expect_run(
        "hot reload",
        code,
        output,
        [
            "hot reload: waiting for version 2",
            "hot reload: waiting for version 3",
            kept_old_code,
            "hot reload: waiting for version 4",
            kept_old_code,
            "hot reload: 0 failure(s)",
        ],
    )


def run_export_groups(godot):
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


if __name__ == "__main__":
    main()
