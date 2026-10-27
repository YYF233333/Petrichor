"""Engine tests: run the test project headless with a Godot 4.7 editor binary.

Usage:
    scons target=editor
    scons target=template_debug    (for the export group)
    python tests/engine/run.py <godot editor binary> [group...]    (or set GODOT)

Checks the game run, the editor side (placeholders, saving, the undo probe), the
other step-2 probes of the architecture, the language, the debugger, hot reload,
an exported pack run in a directory without sources with the library of export
templates, and the FPS starter kit with its migrated scripts. Naming groups runs
only those, after the build; the groups are those of GROUPS, hot reload being
part of debugger.
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
ROOT = HERE.parent.parent
PROJECT = HERE / "project"
TEMPLATE_LIBRARY = "petrichor.windows.template_debug.x86_64.dll"
FPS_UPSTREAM = ROOT / "thirdparty" / "starter-kit-fps"
FPS_MIGRATED = HERE / "fps"
HOT_RELOAD = PROJECT / "language" / "hot_reload"
HOT_RELOAD_BYTECODE = PROJECT / ".godot" / "petrichor" / "language" / "hot_reload"
STALE = PROJECT / "language" / "stale"
STALE_BYTECODE = PROJECT / ".godot" / "petrichor" / "language" / "stale"
LIVE_SCRIPT = "res://language/hot_reload/live.pet"

GAME_OUTPUT = [
    "check: message = from the scene",
    "check: Hello, from the scene",
    "check: Hello, set from GDScript",
    'check: has_method("greet") = true',
    "greeter: _ready, message = set from GDScript",
]

GROUPS = ["game", "editor", "probes", "language", "debugger", "export", "fps"]

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
        # Captured output is bytes here despite text=True, and a stream with
        # nothing captured is None.
        def text(stream):
            return stream.decode("utf-8", "replace") if isinstance(stream, bytes) else stream or ""

        return -1, text(timeout.stdout) + text(timeout.stderr) + "\n(timed out)"
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
    738-741, 776-801); the game reloads it at the end of the idle poll that
    reads the message (remote_debugger.cpp:663-719). A message of the test's
    own follows, which tells the scene that the reload has been handled."""
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
                    send_editor_message(connection, "hot_reload_test:reloaded", [])
                return running.finish()
        finally:
            (HOT_RELOAD / "live.pet").unlink(missing_ok=True)
            (HOT_RELOAD_BYTECODE / "live.petc").unlink(missing_ok=True)


def run_stale_bytecode(godot):
    """Spec §8.1: a call to a method of this language checks that the callee
    still has the signature the caller was compiled against. Within a run that
    always holds (§12.4); a run from bytecode that was not built together, here
    callee.pet replaced by another version while caller.pet is not built again,
    is the way to break it."""
    code, output = run(godot, "--path", str(PROJECT), "res://language/stale/stale_test.tscn")
    expect_run("stale bytecode: as built", code, output, ["stale: 2"])
    originals = {path: path.read_bytes() for path in (STALE / "callee.pet", STALE_BYTECODE / "callee.petc")}
    try:
        shutil.copy(STALE / "callee_changed.pet", STALE / "callee.pet")
        shutil.copy(STALE_BYTECODE / "callee_changed.petc", STALE_BYTECODE / "callee.petc")
        code, output = run(godot, "--path", str(PROJECT), "res://language/stale/stale_test.tscn")
    finally:
        for path, data in originals.items():
            path.write_bytes(data)
    changed = "value() is not the method this script was compiled against. Build the project again."
    expect("stale bytecode: a call to a changed signature is a program error", code == 0 and changed in output and "stale: 0" in output, output)


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
    # A headless run draws nothing, so textures are imported without VRAM
    # compression, on which an unoptimized engine spent most of the import
    # (skybox.png to BPTC took about 40 s).
    for settings in project.rglob("*.import"):
        text = settings.read_text(encoding="utf-8")
        if 'importer="texture"' in text:
            settings.write_text(re.sub(r"^compress/mode=2$", "compress/mode=0", text, flags=re.M), encoding="utf-8")
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
    (architecture §5 step 4); combat.pet has none."""
    replaced = {}
    for script in FPS_MIGRATED.rglob("*.pet"):
        relative = script.relative_to(FPS_MIGRATED)
        original = project / relative.with_suffix(".gd")
        original.unlink(missing_ok=True)
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
    if only - set(GROUPS):
        sys.exit(f"unknown group(s): {' '.join(sorted(only - set(GROUPS)))}; the groups are {' '.join(GROUPS)}")

    def wanted(group):
        return not only or group in only

    # The fps group prepares projects of its own.
    if not only or only - {"fps"}:
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
        # Verbose, so that StringNames still referenced at exit are listed.
        code, output = run(godot, "--verbose", "--path", str(PROJECT), "res://language/language.tscn")
        expect_run("language", code, output, ["language: 0 failure(s)"])
        expect("language: no value leaks", "Orphan StringName: petrichor_leak_probe" not in output, output)
        run_stale_bytecode(godot)

    if wanted("debugger"):
        run_debugger_groups(godot)

    if wanted("export"):
        run_export_groups(godot)

    if wanted("fps"):
        run_fps_group(godot)

    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


def resources_in_use(output):
    """What a --verbose run reports as still in use at exit, apart from sounds:
    a stopped sound's playback is released only when the audio thread next
    mixes, so which sounds are still held at exit depends on timing."""
    return {line for line in output.splitlines() if line.startswith("Resource still in use: ") and "res://sounds/" not in line}


def run_fps_group(godot):
    # Architecture §5 step 4: the starter kit runs with its migrated scripts.
    # The original reports errors of its own in a headless run (sounds still
    # playing at exit, and weapon models it removes without freeing), so the
    # migrated run may only repeat those, and keep no resource at exit, other
    # than sounds, that the original does not.
    with tempfile.TemporaryDirectory() as tmp:
        original = Path(tmp) / "original"
        prepare_fps(original)
        run(godot, "--path", str(original), "--import")
        code, output = run(godot, "--verbose", "--path", str(original))
        expect("fps original run", code == 0 and "fps check: 0 failure(s)" in output, output)
        known_errors = {line for line in output.splitlines() if "ERROR" in line}
        known_in_use = resources_in_use(output)

        # Prepared afresh: the original's .godot caches its global classes,
        # such as Weapon, with the paths of the GDScript files. The assets are
        # the same, so their imported data is reused instead of imported again.
        migrated = Path(tmp) / "migrated"
        prepare_fps(migrated)
        shutil.copytree(original / ".godot" / "imported", migrated / ".godot" / "imported")
        migrate_fps(migrated)
        run(godot, "--path", str(migrated), "--import")
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-build")
        expect_run("fps build", code, output)
        expect("fps build: every script compiles", ", 0 with errors" in output, output)
        code, output = run(godot, "--verbose", "--path", str(migrated))
        new_errors = [
            line for line in output.splitlines()
            if "ERROR" in line and line not in known_errors and not re.match(r"ERROR: \d+ resources still in use at exit", line)
        ]
        expect("fps run", code == 0 and not new_errors and "fps check: 0 failure(s)" in output, output)
        expect("fps run: no resources kept at exit that the original frees", resources_in_use(output) <= known_in_use, output)
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
        commands=["locals", "members", "s", "members", "c", "c", "locals", "c"],
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
            "Debugger Break, Reason: 'Breakpoint'",
            "*Frame 0 - res://language/breakpoint.pet:8 in function 'step'",
            "n: 3",
            "breakpoint: step on a thread returned 7",
        ],
    )

    # Spec §12.4: a body change replaces the code of running instances; after
    # any other change the game keeps its code, and applies no later reload
    # until it runs again.
    code, output = run_hot_reload(godot)
    expect_run(
        "hot reload",
        code,
        output,
        [
            "hot reload: waiting for version 2",
            "hot reload: waiting for version 3",
            f"{LIVE_SCRIPT}: more than function bodies changed, so the running game keeps its code, and reloads no script until it runs again.",
            "hot reload: waiting for version 4",
            f"{LIVE_SCRIPT}: not reloaded, since an earlier change needs the game to run again.",
            "hot reload: 0 failure(s)",
        ],
    )


def run_export_groups(godot):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pack = tmp / "game.pck"
        # A script with errors is left out, not exported as source (spec §1.3).
        broken = PROJECT / "broken_export_check.pet"
        broken.write_text('extends Node\n\nfunc f() -> int:\n\treturn "not an int"\n', encoding="utf-8")
        try:
            code, output = run(godot, "--path", str(PROJECT), "--export-pack", "Windows Desktop", str(pack))
        finally:
            broken.unlink(missing_ok=True)
            broken.with_name(broken.name + ".uid").unlink(missing_ok=True)
        expected = ("broken_export_check.pet", "Cannot return a value of type String as int.")
        unexpected = [line for line in output.splitlines() if "ERROR" in line and not any(part in line for part in expected)]
        expect("export", code == 0 and not unexpected, output)
        expect("export: a script with errors is left out", "broken_export_check.pet has errors and is left out of the export." in output, output)

        # A project directory without sources, whose only library is the one
        # exported games load, without the compiler: without the editor's
        # entry, an editor binary takes the "debug" one
        # (core/extension/gdextension_library_loader.cpp:82-96).
        if not template_library_current():
            expect("exported pack run: the template_debug library is built from the current sources", False, "Build it with: scons target=template_debug")
            return
        runner = tmp / "runner"
        (runner / ".godot").mkdir(parents=True)
        (runner / "bin").mkdir()
        shutil.copy(PROJECT / "project.godot", runner)
        shutil.copy(PROJECT / ".godot" / "extension_list.cfg", runner / ".godot")
        shutil.copy(PROJECT / "bin" / TEMPLATE_LIBRARY, runner / "bin")
        extension = (PROJECT / "petrichor.gdextension").read_text(encoding="utf-8").splitlines()
        (runner / "petrichor.gdextension").write_text("\n".join(line for line in extension if not line.startswith("windows.editor.")), encoding="utf-8")
        code, output = run(godot, "--path", str(runner), "--script", str(HERE / "export_check.gd"), "--", str(pack))
        expect_run("exported pack run with the template_debug library", code, output, ["export check: greeter.petc header = PETC", *GAME_OUTPUT])


def template_library_current():
    """Whether the template_debug library is built from the current sources;
    scons -q only answers, without building."""
    scons = shutil.which("scons")
    return scons is not None and subprocess.run([scons, "target=template_debug", "-q"], cwd=ROOT, capture_output=True).returncode == 0


if __name__ == "__main__":
    main()
