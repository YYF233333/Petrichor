"""Engine tests: run the test project headless with a Godot 4.7 editor binary.

Usage:
    scons target=editor
    scons target=template_debug      (for the export group)
    scons target=template_release    (for the export group)
    python tests/engine/run.py <godot editor binary> [group...]    (or set GODOT)

Checks the game run, the editor side (placeholders, saving), the language, the
debugger, hot reload, an exported pack run in a directory without sources with
each library of export templates, the language server, and the FPS starter
kit and Ring's breakout with their migrated scripts. Naming groups runs only
those, after the build; the groups are those of GROUPS, hot reload being part
of debugger. The dotnet group, classes written in C#, runs on a .NET editor or
when named; it needs the .NET SDK 8 or later on PATH.

Environment:
    PETRICHOR_PREBUILT=1          the libraries in tests/engine/project/bin were
                                  built elsewhere: they only have to exist
    PETRICHOR_TEST_TIMEOUT=<s>    the limit of each engine run, 60 s by default
"""

import configparser
import hashlib
import json
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
PROJECT = HERE / "project"
BUILD_STAMP = PROJECT / ".godot" / "petrichor_build_stamp"
FPS_UPSTREAM = ROOT / "thirdparty" / "starter-kit-fps"
FPS_MIGRATED = HERE / "fps"
RING_UPSTREAM = ROOT / "thirdparty" / "ring"
RING_MIGRATED = HERE / "ring"
HOT_RELOAD = PROJECT / "language" / "hot_reload"
HOT_RELOAD_BYTECODE = PROJECT / ".godot" / "petrichor" / "language" / "hot_reload"
STALE = PROJECT / "language" / "stale"
STALE_BYTECODE = PROJECT / ".godot" / "petrichor" / "language" / "stale"
LIVE_SCRIPT = "res://language/hot_reload/v1.pet"

GAME_OUTPUT = [
    "check: message = from the scene",
    "check: Hello, from the scene",
    "check: Hello, set from GDScript",
    'check: has_method("greet") = true',
    "greeter: _ready, message = set from GDScript",
]

GROUPS = ["game", "editor", "language", "debugger", "export", "lsp", "pet", "fps", "ring", "dotnet"]
LSP = HERE / "lsp"
DOTNET = HERE / "dotnet"
DOTNET_ASSEMBLY = Path(".godot", "mono", "temp", "bin", "Debug", "DotnetCheck.dll")

# The host's export platform, and its features by which the engine picks a
# library (core/os/os.cpp:471-556; ed1daf0).
HOST_PLATFORM, HOST_FEATURES = {
    "win32": ("Windows Desktop", {"windows"}),
    "linux": ("Linux", {"linux"}),
    "darwin": ("macOS", {"macos", "universal"}),
}[sys.platform]
HOST_FEATURES = HOST_FEATURES | {{"amd64": "x86_64", "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())}
PREBUILT = os.environ.get("PETRICHOR_PREBUILT") == "1"

failures = []


TIMEOUT = float(os.environ.get("PETRICHOR_TEST_TIMEOUT", 60))
PROMPT = b"debug> "
# A scene whose GDScript failed to load never quits, and no run expects it.
GDSCRIPT_NOT_LOADED = re.compile(rb'Failed to load script "[^"]*\.gd"')


def run(godot, *args):
    """Runs to the end. A scene is stopped early when a GDScript file fails to
    load, and by the timeout when another script error keeps it from quitting.
    The editor (-e) quits by itself."""
    running = Running(godot, *args)
    scanned = 0
    found = None

    def ended_or_broken():
        nonlocal scanned, found
        if "-e" not in args and not found:
            found = GDSCRIPT_NOT_LOADED.search(running.output, max(0, scanned - 256))
        scanned = len(running.output)
        return running.ended or found

    with running.changed:
        running.changed.wait_for(ended_or_broken, timeout=running._remaining())
        broken = found and not running.ended
    if broken:
        running.process.kill()
    code, output = running.finish()
    return (-1, output + "\n(stopped: a GDScript file failed to load)") if broken else (code, output)


class Running:
    """A run whose output is read while it goes on, so that the test can answer it."""

    def __init__(self, godot, *args, stdin=False):
        # In English: the checks read the editor's texts, which otherwise take
        # the language of the editor's settings or of the system
        # (editor/settings/editor_settings.cpp:1458-1465, 2031-2059; ed1daf0).
        self.process = subprocess.Popen(
            [godot, "--headless", "--language", "en", *args],
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


def run_evaluate(godot):
    """Stands in for the editor's evaluator at the breakpoint in step() of
    breakpoint.pet, with an expression that reads a member. The answer holds
    the expression, 3, the value's type and the value (remote_debugger.cpp:549-618,
    debugger_marshalls.cpp:65-88). Then the game runs on with breakpoints and
    error breaks off. Error breaks are off before the expression too: the
    engine loads every global class for it (:598-607), and the static
    initialization of one of the project's fails. Gives the run, and whether
    the answer came in the break."""
    answer = b"".join(encode_variant(value) for value in ["calls + 4321", 3, 2, 4321])
    with socket.create_server(("127.0.0.1", 0)) as server:
        server.settimeout(TIMEOUT)
        running = Running(
            godot,
            "--remote-debug",
            f"tcp://127.0.0.1:{server.getsockname()[1]}",
            "--breakpoints",
            "res://language/breakpoint.pet:8",
            "--path",
            str(PROJECT),
            "res://language/breakpoint.tscn",
        )
        try:
            connection, _ = server.accept()
        except OSError:
            return (*running.finish(), False)
        with connection:
            connection.settimeout(TIMEOUT)
            received = bytearray()

            def receive(data):
                try:
                    while data not in received:
                        chunk = connection.recv(65536)
                        if not chunk:
                            return False
                        received.extend(chunk)
                except OSError:
                    return False
                return True

            answered = receive(encode_variant("debug_enter"))
            send_editor_message(connection, "set_ignore_error_breaks", [1])
            if answered:
                send_editor_message(connection, "evaluate", ["calls + 4321", 0])
                answered = receive(answer) and b"step returned" not in running.output
            send_editor_message(connection, "set_skip_breakpoints", [1])
            send_editor_message(connection, "continue", [])
            threading.Thread(target=drain, args=(connection,), daemon=True).start()
            return (*running.finish(), answered)


def snapshot_files(paths):
    return {path: (path.read_bytes(), path.stat()) for path in paths}


def restore_files(originals):
    for path, (data, stat) in originals.items():
        path.write_bytes(data)
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))


def run_hot_reload(godot):
    """Stands in for the editor while hot_reload_test.gd runs connected to it.

    When the scene asks for a version, it is put in place of v1.pet with its
    bytecode, which is what saving it in the editor leaves on disk
    (architecture §2.4), and announced with reload_scripts, as the editor does
    after a save (script_editor_plugin.cpp:738-741, 776-801); the game reloads
    it at the end of the idle poll that reads the message
    (remote_debugger.cpp:663-719). A message of the test's own follows, which
    tells the scene that the reload has been handled."""
    live = (HOT_RELOAD / "v1.pet", HOT_RELOAD_BYTECODE / "v1.petc")
    originals = snapshot_files(live)
    with socket.create_server(("127.0.0.1", 0)) as server:
        server.settimeout(TIMEOUT)
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
                    shutil.copy(HOT_RELOAD / f"v{version}.pet", live[0])
                    # Version 4 keeps the bytecode of version 3, as when the editor could
                    # not compile it against the files: after a refusal the game reads none.
                    if version < 4:
                        shutil.copy(HOT_RELOAD_BYTECODE / f"v{version}.petc", live[1])
                    send_editor_message(connection, "reload_scripts", [LIVE_SCRIPT])
                    send_editor_message(connection, "hot_reload_test:reloaded", [])
                return running.finish()
        finally:
            restore_files(originals)


def run_stale_bytecode(godot):
    """Spec §8.1: a call to a method of this language checks that the callee
    still has the signature the caller was compiled against, a member or a
    static variable of another script the type, and a class loads only with
    the base it was compiled against. Within a run that always holds
    (§12.4); a run from bytecode that was not built together, here callee.pet,
    inherit_base.pet and layout_base.pet replaced by other versions while the
    scripts that use them are not built again, is the way to break it."""
    code, output = run(godot, "--path", str(PROJECT), "res://language/stale/stale_test.tscn")
    expect_run("stale bytecode: as built", code, output, ["stale: 2", "stale member: true static: true", "stale inheritance: valid: 1", "stale layout: valid: 1"])
    replaced = ("callee", "inherit_base", "layout_base")
    originals = snapshot_files(path for name in replaced for path in (STALE / f"{name}.pet", STALE_BYTECODE / f"{name}.petc"))
    try:
        for name in replaced:
            shutil.copy(STALE / f"{name}_changed.pet", STALE / f"{name}.pet")
            shutil.copy(STALE_BYTECODE / f"{name}_changed.petc", STALE_BYTECODE / f"{name}.petc")
        code, output = run(godot, "--path", str(PROJECT), "res://language/stale/stale_test.tscn")
    finally:
        restore_files(originals)
    changed = "value() is not the method this script was compiled against. Build the project again."
    expect("stale bytecode: a call to a changed signature is a program error", code == 0 and changed in output and "stale: 0" in output, output)
    retyped = ['"held" is not the member this script was compiled against.', "A static variable of res://language/stale/callee.pet is not the one this script was compiled against."]
    expect("stale bytecode: a member or static variable whose type changed is a program error", all(text in output for text in retyped) and "stale member: false static: false" in output, output)
    refused = "inherit_derived.pet Base method declaration changed. Rebuild the project."
    expect("stale bytecode: a class whose base changed is refused", refused in output and "stale inheritance: rejected" in output, output)
    relaid = "layout_derived.pet Inherited member layout changed. Rebuild the project."
    expect("stale bytecode: a class whose base's member must now be assigned is refused", relaid in output and "stale layout: rejected" in output, output)


def drain(connection):
    # The game's messages to the editor are not checked here.
    try:
        while connection.recv(65536):
            pass
    except OSError:
        pass


def import_project(godot, project):
    """Imports the project without checking the import. In Godot 4.7.2 an
    editor that exits right after its work, as --import does, can run the
    documentation callbacks it queued (editor/doc/editor_help.cpp:3038-3044,
    3077) after it freed the documentation (:3365-3368; main/main.cpp:5257),
    and crash, or first report an error from EditorNode::is_cmdline_mode().
    It does when it found the extension only while scanning, as in a fresh
    copy, and when its documentation cache was written by another build, as
    the dev and official editors share it (:2952-2959). The import is saved by
    then, and the runs that use it are checked. Godot issue #111048; check the
    import again once the engine has pull request #123658."""
    run(godot, "--path", str(project), "--import")


def copy_project(upstream, project, ignore):
    """A copy of a real project, with the extension."""
    shutil.copytree(upstream, project, ignore=ignore)
    # A headless run draws nothing, so textures are imported without VRAM
    # compression, on which an unoptimized engine spent most of the import
    # (the FPS kit's skybox.png to BPTC took about 40 s).
    for settings in project.rglob("*.import"):
        text = settings.read_text(encoding="utf-8")
        if 'importer="texture"' in text:
            settings.write_text(re.sub(r"^compress/mode=2$", "compress/mode=0", text, flags=re.M), encoding="utf-8")
    shutil.copytree(PROJECT / "bin", project / "bin")
    shutil.copy(PROJECT / "petrichor.gdextension", project)


def prepare_fps(project):
    """The FPS starter kit with the extension and fps_check.gd."""
    copy_project(FPS_UPSTREAM, project, shutil.ignore_patterns(".git"))
    shutil.copy(FPS_MIGRATED / "fps_check.gd", project)

    settings = project / "project.godot"
    text = settings.read_text(encoding="utf-8")
    text = re.sub(r"^\[autoload\]\r?$", '[autoload]\n\nFpsCheck="*res://fps_check.gd"', text, count=1, flags=re.M)
    if "FpsCheck" not in text:
        text += '\n[autoload]\n\nFpsCheck="*res://fps_check.gd"\n'
    settings.write_text(text, encoding="utf-8")


def prepare_ring(project):
    """Ring with the extension and ring_check.tscn, which starts breakout in
    place of Ring's C#. Of assets/, the visual novel's, only the bricks that
    breakout uses and the textures of the project's theme are copied; the
    visual novel's other directories, DialogueManager's editor plugin and 2 MB
    main_view.tscn, shaderV's example scene, unused breakout source archives
    and drafts, and the documentation are left out. UI/1.png alone took half
    the import."""
    mine_used = {
        "blood bullet.png",
        "calculator.png",
        "communicator.png",
        "new model.png",
        "notebook.png",
        "old model.png",
        "package.png",
        "passport.png",
        "pixil-glowing-blue.png",
        "select_box.png",
    }

    def ignore(directory, names):
        path = Path(directory)
        if path == RING_UPSTREAM:
            return [name for name in names if name in (".git", "docs", "Runtime", "button", "scenes", "UI", "Test", "python")]
        if path == RING_UPSTREAM / "addons" / "dialogue_manager":
            return [name for name in names if name in ("views", "plugin.cfg", "plugin.gd", "plugin.gd.uid")]
        if path == RING_UPSTREAM / "assets":
            return [name for name in names if not name.startswith("Brick-") and name != "Runtime"]
        if path == RING_UPSTREAM / "breakout":
            return [name for name in names if name in (".git", "docs", "NOTE.assets", "README.assets")]
        if path == RING_UPSTREAM / "breakout" / "addons" / "shaderV" / "examples":
            return [name for name in names if name.startswith("basic_examples.tscn")]
        if path == RING_UPSTREAM / "breakout" / "assets":
            return [name for name in names if name == "Breakout_Tile_Set_Free.zip"]
        if path == RING_UPSTREAM / "breakout" / "assets" / "Breakout_Tile_Set_Free" / "Breakout Tile Set Free":
            return [name for name in names if name in ("Sprite Sheet", "Vector")]
        if path == RING_UPSTREAM / "breakout" / "assets" / "Mine":
            return [name for name in names if name.removesuffix(".import") not in mine_used]
        return [name for name in names if name in (".git", "docs") or name.endswith((".dialogue", ".dialogue.uid", ".tmp"))]

    copy_project(RING_UPSTREAM, project, ignore)
    for file in ("ring_check.gd", "ring_check.tscn"):
        shutil.copy(RING_MIGRATED / file, project)
    settings = project / "project.godot"
    text = settings.read_text(encoding="utf-8")
    text = re.sub(r'^run/main_scene=.*$', 'run/main_scene="res://ring_check.tscn"', text, flags=re.M)
    text = re.sub(r'^enabled=PackedStringArray\("res://addons/dialogue_manager/plugin\.cfg"\)\r?\n?', "", text, flags=re.M)
    text = re.sub(r'^locale/translations_pot_files=.*\r?\n?', "", text, flags=re.M)
    settings.write_text(text, encoding="utf-8")


def migrate(project, migrated):
    """Puts each migrated script in place of its GDScript original
    (architecture §5 steps 4 and 6); a script without one, such as the FPS
    kit's combat.pet, is added."""
    replaced = {}
    for script in migrated.rglob("*.pet"):
        relative = script.relative_to(migrated)
        original = project / relative.with_suffix(".gd")
        original.unlink(missing_ok=True)
        original.with_name(original.name + ".uid").unlink(missing_ok=True)
        shutil.copy(script, project / relative)
        replaced["res://" + relative.with_suffix(".gd").as_posix()] = "res://" + relative.as_posix()

    # Scenes and resources name a script by uid and path; the uid belonged to
    # the original, so only the path stays. Line ends are kept, so that the
    # demo package is the same on every host.
    for file in [*project.rglob("*.tscn"), *project.rglob("*.tres"), project / "project.godot"]:
        lines = file.read_bytes().decode("utf-8").split("\n")
        for i, line in enumerate(lines):
            for old, new in replaced.items():
                if f'"{old}"' in line or f'"*{old}"' in line:
                    lines[i] = re.sub(r' uid="[^"]*"', "", line).replace(old, new)
        file.write_bytes("\n".join(lines).encode("utf-8"))


report = threading.local()


def say(text):
    """Prints, or keeps the text while a group runs beside the others."""
    kept = getattr(report, "lines", None)
    if kept is None:
        print(text, flush=True)
    else:
        kept.append(text)


def expect(name, ok, output=""):
    say(("ok      " if ok else "FAILED  ") + name)
    if not ok:
        failures.append(name)
        if output:
            say(output)


def beside(name, function, *args):
    """Starts a group that uses its own copy of a project, beside the groups of
    the test project; finish() waits for it and prints what it reported."""
    lines = []

    def body():
        report.lines = lines
        try:
            function(*args)
        except Exception:
            import traceback
            expect(f"{name}: the group ran to its end", False, traceback.format_exc())

    thread = threading.Thread(target=body)
    thread.start()

    def finish():
        thread.join()
        for line in lines:
            print(line, flush=True)

    return finish


def expect_run(name, code, output, lines=(), expected_errors=()):
    """The run exits with 0, reports no errors other than `expected_errors`,
    each as often as it is listed, and prints `lines` in order.

    A line or an error may follow a prompt, such as the debugger's "debug> "."""
    allowed = Counter(expected_errors)
    errors = []
    for line in output.splitlines():
        if "ERROR" not in line:
            continue
        text = re.sub(r"^.*?\b(SCRIPT ERROR|ERROR): ", "", line)
        if allowed[text] > 0:
            allowed[text] -= 1
        else:
            errors.append(line)
    remaining = iter(line.strip() for line in output.splitlines())
    in_order = all(any(out.endswith(line) for out in remaining) for line in lines)
    ok = code == 0 and not errors and not any(allowed.values()) and in_order
    expect(name, ok, output)
    return ok


def expected_errors(output, prefix):
    """The errors a check expected: error_log.gd prints each error a check
    compared with the ones it expects as "<prefix>: expected error: <message>"."""
    marker = f"{prefix}: expected error: "
    return [line.split(marker, 1)[1] for line in output.splitlines() if line.startswith(marker)]


def main():
    # The engine prints characters that a console of a Windows code page lacks.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")
    godot = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GODOT")
    if not godot:
        sys.exit(__doc__)
    only = set(sys.argv[2:])
    if only - set(GROUPS):
        sys.exit(f"unknown group(s): {' '.join(sorted(only - set(GROUPS)))}; the groups are {' '.join(GROUPS)}")

    def wanted(group):
        return not only or group in only

    editor_library = PROJECT / host_library("editor")
    if not editor_library.is_file():
        sys.exit(f"{editor_library} is missing. Build it with: scons")

    # The lsp, pet, fps and ring groups prepare projects of their own, so they
    # run beside the groups of the test project.
    finishes = []
    if wanted("lsp"):
        finishes.append(beside("lsp", run_lsp_group, godot))
    if wanted("pet"):
        finishes.append(beside("pet", run_pet_group, godot))
    if wanted("fps"):
        finishes.append(beside("fps", run_migration_group, godot, "fps", prepare_fps, FPS_MIGRATED, True))
    if wanted("ring"):
        finishes.append(beside("ring", run_migration_group, godot, "ring", prepare_ring, RING_MIGRATED, False, "res://ring_check.tscn"))
    dotnet = dotnet_build(godot)
    if "dotnet" in only or (not only and dotnet):
        finishes.append(beside("dotnet", run_dotnet_group, godot, dotnet))
    elif not only:
        print(f"dotnet: skipped, as {godot} is not a .NET build")

    if not only or (only - {"lsp", "pet", "fps", "ring", "dotnet"} and ("editor" in only or not project_bytecode_current(godot))):
        # The game reads the bytecode the editor writes (architecture §2.4).
        # The checked build runs on an imported project, as import_project()
        # explains.
        if not (PROJECT / ".godot").is_dir():
            import_project(godot, PROJECT)
        BUILD_STAMP.unlink(missing_ok=True)
        code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--petrichor-build")
        build_ok = expect_run("build", code, output)
        compiled = ", 0 with errors" in output
        expect("build: every script compiles", compiled, output)
        if build_ok and compiled:
            stamp = project_build_stamp(godot)
            if stamp is not None:
                BUILD_STAMP.write_text(stamp + "\n", encoding="utf-8")

    if wanted("game"):
        code, output = run(godot, "--path", str(PROJECT))
        expect_run("game run", code, output, GAME_OUTPUT)

    if wanted("editor"):
        # The editor opens again the scenes, scripts and help pages it last
        # saved as open, which it saves 0.5 s after they change. A help page
        # left so fails the documentation check of the next run
        # (editor/editor_node.cpp:6311-6312, 8823-8827,
        # editor/script/script_editor_plugin.cpp:3208-3209; ed1daf0).
        (PROJECT / ".godot" / "editor" / "editor_layout.cfg").unlink(missing_ok=True)
        code, output = run(godot, "-e", "--path", str(PROJECT), "--", "--editor-checks")
        expect_run("editor checks", code, output, ["editor check: 0 failure(s)"], expected_errors(output, "editor check"))

    if wanted("language"):
        # Verbose, so that StringNames still referenced at exit are listed.
        code, output = run(godot, "--verbose", "--path", str(PROJECT), "res://language/language.tscn")
        expect_run("language", code, output, ["language: 0 failure(s)"], expected_errors(output, "language"))
        expect("language: no value leaks", "Orphan StringName: petrichor_leak_probe" not in output, output)
        # Spec §6.7, §12.5: the live instances of this language and what its
        # new() made, listed at exit.
        code, output = run(godot, "--path", str(PROJECT), "res://language/leak.tscn")
        expect_run("leaks at exit", code, output, [
            "WARNING: 4 objects of this language leaked at exit, at 3 places:",
            "2 × LeakPair, made at res://language/leak_check.pet:18 in pair()",
            "1 × AccessorCheck, made outside this language",
            "1 × Node, made at res://language/leak_check.pet:8 in _ready()",
        ], ["2 resources still in use at exit (run with --verbose for details)."])
        run_stale_bytecode(godot)

    if wanted("debugger"):
        run_debugger_groups(godot)

    if wanted("export"):
        run_export_groups(godot)

    for finish in finishes:
        finish()

    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


def resources_in_use(output):
    """What a --verbose run reports as still in use at exit, apart from sounds:
    a stopped sound's playback is released only when the audio thread next
    mixes, so which sounds are still held at exit depends on timing."""
    return {line for line in output.splitlines() if line.startswith("Resource still in use: ") and not re.search(r"\.(ogg|wav|mp3)\b", line)}


def run_migration_group(godot, name, prepare, migrated_scripts, check_format, *scene):
    """A real project runs with its migrated scripts (architecture §5 steps 4
    and 6). The original reports errors of its own in a headless run, such as
    the FPS kit's sounds still playing at exit and weapon models it removes
    without freeing, so the migrated run may only repeat those, and keep no
    resource at exit, other than sounds, that the original does not. With
    `check_format`, the format check first fails on the scripts written by
    hand (README)."""
    # A headless frame sleeps, as nothing is drawn (core/os/os.cpp:721-723),
    # unless the frame rate is fixed (main/main.cpp:5170-5172); fixed, each
    # frame advances the game by 1/60 s, however fast it runs.
    scene = ("--fixed-fps", "60", *scene)
    with tempfile.TemporaryDirectory() as tmp:
        original = Path(tmp) / "original"
        prepare(original)
        scripts = sorted(migrated_scripts.rglob("*.pet"))
        if not scripts:
            import_project(godot, original)
        else:
            # Trigger the same scene analysis as a migrated script; this file
            # is never attached to a node and the original game stays GDScript.
            # The build imports the copy in its first scan.
            baseline = original / "petrichor_analysis_baseline.pet"
            assert not baseline.exists()
            baseline.write_text("extends Node\n", encoding="utf-8")
            code, output = run(godot, "-e", "--path", str(original), "--", "--petrichor-build")
            baseline.unlink()
            baseline.with_name(baseline.name + ".uid").unlink(missing_ok=True)
            # Not its exit code: the first editor run of a fresh copy can crash
            # at exit, as import_project() explains, and does when the build is
            # quick. The migrated build below starts from the imported copy and
            # is checked in full.
            expect(f"{name} original analysis baseline", ", 0 with errors" in output, output)
            known_build_errors = {line for line in output.splitlines() if "ERROR" in line}
        code, output = run(godot, "--verbose", "--path", str(original), *scene)
        expect(f"{name} original run", code == 0 and f"{name} check: 0 failure(s)" in output, output)
        if not scripts:
            return
        known_errors = {line for line in output.splitlines() if "ERROR" in line}
        known_in_use = resources_in_use(output)

        # The original as imported, so that no asset is imported again, which
        # a fresh copy would do even with .godot/imported (its .import files
        # differ from the ones the import rewrote); without the cache of global
        # classes, such as the FPS kit's Weapon, which names the GDScript files.
        migrated = Path(tmp) / "migrated"
        shutil.copytree(original, migrated)
        (migrated / ".godot" / "global_script_class_cache.cfg").unlink()
        migrate(migrated, migrated_scripts)
        # Spec §13.2: the migrated game is built and run formatted; the
        # scripts written by hand are not. A byte order mark is no text, and
        # stays.
        bom = migrated / "bom_check.pet"
        bom.write_bytes(b"\xef\xbb\xbfextends Node\nvar x=1\n")
        if check_format:
            code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-format", "--check")
            unformatted = re.search(r", (\d+) not formatted,", output)
            expect(f"{name} format check: they are not formatted yet", code == 1 and ", 0 with errors" in output and unformatted and int(unformatted.group(1)) > 1, output)
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-format")
        changed = re.search(r", (\d+) formatted,", output)
        expect(f"{name} format", code == 0 and ", 0 with errors" in output and changed and int(changed.group(1)) > 0, output)
        formatted_bom = bom.read_bytes()
        expect(f"{name} format: a byte order mark stays", formatted_bom.startswith(b"\xef\xbb\xbfextends") and b"var x = 1" in formatted_bom, formatted_bom.decode("utf-8", "replace"))
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-build")
        new_build_errors = [line for line in output.splitlines() if "ERROR" in line and line not in known_build_errors]
        expect(f"{name} build", code == 0 and not new_build_errors, "\n".join(new_build_errors) if code == 0 else output)
        expect(f"{name} build: every script compiles", ", 0 with errors" in output, output)
        code, output = run(godot, "--verbose", "--path", str(migrated), *scene)
        new_errors = [
            line for line in output.splitlines()
            if "ERROR" in line and line not in known_errors and not re.match(r"ERROR: \d+ resources still in use at exit", line)
        ]
        expect(f"{name} run", code == 0 and not new_errors and f"{name} check: 0 failure(s)" in output, output)
        expect(f"{name} run: no resources kept at exit that the original frees", resources_in_use(output) <= known_in_use, output)
        for script in scripts:
            expect(f"{name} run: {script.stem} runs migrated", f"{name} check: {script.stem} runs PetScript" in output, output)


def dotnet_build(godot):
    """Whether the editor is a .NET build, whose version names the mono module
    (modules/mono/config.py:17, methods.py:132-153, core/version.h:62-70;
    ed1daf0), as in 4.7.2.stable.mono.official.ed1daf0bf."""
    version = subprocess.run([godot, "--version"], capture_output=True, text=True).stdout
    return "mono" in version.strip().split(".")


def run_dotnet_group(godot, dotnet):
    """Spec §11.1 and §11.3 with classes written in C#, in a copy of
    tests/engine/dotnet: a .NET editor writes its solution and builds into
    the project it opens. The editor builds the C# with the .NET SDK
    (main/main.cpp:5181-5194; ed1daf0), then this language's scripts."""
    if not dotnet:
        expect("dotnet: the editor is a .NET build", False, f"{godot} is not. The group needs Godot 4.7's .NET editor.")
        return
    if shutil.which("dotnet") is None:
        expect("dotnet: the .NET SDK is on PATH", False, "dotnet was not found. The editor builds the group's C# with the .NET SDK 8 or later.")
        return
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp) / "dotnet"
        shutil.copytree(DOTNET, project)
        (project / "bin").mkdir()
        for file in ("petrichor.gdextension", "error_log.gd", host_library("editor")):
            shutil.copy(PROJECT / file, project / file)
        code, output = run(godot, "-e", "--path", str(project), "--build-solutions", "--quit")
        if not (project / DOTNET_ASSEMBLY).is_file():
            expect("dotnet: the editor builds the C# assembly", False, output)
            return
        # The static view of C# classes refuses what spec §11.1 says it does.
        code, output = run(godot, "-e", "--path", str(project), "--", "--petrichor-build")
        refused = [(message, line) for line, message in re.findall(r"^cs_limits\.pet:(\d+):\d+: error: ([^\r\n]*)", output, flags=re.M)]
        expect("dotnet build: only cs_limits.pet has errors", ", 1 with errors" in output, output)
        expect("dotnet build: a C# class has no constructor arguments and no constants, its object members and results are T?, and its untyped ones Variant", [
            ('Function "new()" takes 0 argument(s), 1 given.', "6"),
            ('Counter has no constant "Limit".', "7"),
            ("Cannot assign a value of type Object? as Node.", "8"),
            ("Cannot assign a value of type Node? as Node.", "9"),
            ("Cannot assign a value of type Variant as int. Convert it first with int(v), or narrow it with `if v is int:`.", "10"),
        ] == refused, output)
        code, output = run(godot, "--path", str(project))
        expect_run("dotnet run", code, output, ["dotnet: 0 failure(s)"], expected_errors(output, "dotnet"))


def file_uri(path, encoded=True):
    """A file: URI as VS Code writes it on Windows, with the drive letter in
    lower case and the colon, spaces and non-ASCII bytes percent-encoded; or
    the path as it is."""
    posix = path.as_posix()
    if not posix.startswith("/"):
        posix = "/" + (posix[0].lower() + posix[1:] if encoded else posix)
    return "file://" + (urllib.parse.quote(posix) if encoded else posix)


def lsp_position(text, index):
    """Where text[index] is as LSP counts: lines that \\r\\n, \\r or \\n end,
    columns in UTF-16 code units."""
    before = text[:index]
    start = max(before.rfind("\n"), before.rfind("\r")) + 1
    return {"line": len(re.findall(r"\r\n|\r|\n", before)), "character": len(before[start:].encode("utf-16-le")) // 2}


def lsp_index(text, position):
    """The index of an LSP position; past a line's end its end, past the last
    line the text's end (LSP 3.17, Position)."""
    breaks = list(re.finditer(r"\r\n|\r|\n", text))
    line, character = position["line"], position["character"]
    if line > len(breaks):
        return len(text)
    index = breaks[line - 1].end() if line > 0 else 0
    end = breaks[line].start() if line < len(breaks) else len(text)
    units = 0
    while index < end and units + len(text[index].encode("utf-16-le")) // 2 <= character:
        units += len(text[index].encode("utf-16-le")) // 2
        index += 1
    return index


CLOSED = object()


class LspClient:
    """A client of the language server, connected as VS Code's is."""

    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.data = b""
        self.kept = []
        self.ids = 0

    def send_bytes(self, data):
        self.socket.sendall(data)

    def send(self, message):
        body = json.dumps({"jsonrpc": "2.0", **message}, ensure_ascii=False).encode()
        self.send_bytes(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))

    def notify(self, method, params):
        self.send({"method": method, "params": params})

    def request(self, method, params=None):
        self.ids += 1
        sent = self.ids
        self.send({"id": sent, "method": method, "params": params})
        return self.take(lambda m: m.get("id") == sent and "method" not in m)

    def _next(self, deadline):
        """The next message; None at the deadline, CLOSED once the server closes."""
        while True:
            header, found, rest = self.data.partition(b"\r\n\r\n")
            length = int(re.search(rb"Content-Length: (\d+)", header).group(1)) if found else -1
            if 0 <= length <= len(rest):
                self.data = rest[length:]
                return json.loads(rest[:length])
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.socket.settimeout(remaining)
            try:
                chunk = self.socket.recv(65536)
            except socket.timeout:
                return None
            except OSError:
                chunk = b""
            if not chunk:
                return CLOSED
            self.data += chunk

    def take(self, wanted, seconds=10):
        """The first message, kept or arriving within `seconds`, that is
        `wanted`; the others are kept."""
        for message in self.kept:
            if wanted(message):
                self.kept.remove(message)
                return message
        deadline = time.monotonic() + seconds
        while (message := self._next(deadline)) not in (None, CLOSED):
            if wanted(message):
                return message
            self.kept.append(message)
        return None

    def closed(self, seconds=10):
        deadline = time.monotonic() + seconds
        while (message := self._next(deadline)) is not None:
            if message is CLOSED:
                return True
            self.kept.append(message)
        return False

    def diagnostics(self, uri, until, seconds=10):
        """The diagnostics published for `uri` once `until` holds for them, or
        else the last ones published."""
        last = None
        deadline = time.monotonic() + seconds
        while message := self.take(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri, deadline - time.monotonic()):
            last = message["params"]
            if until(last):
                break
        return last

    def open(self, uri, text, version=1):
        self.notify("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "petrichor", "version": version, "text": text}})

    def initialize(self, annotations=True, view=None):
        document = {"completion": {"completionItem": {"snippetSupport": True}}, "codeAction": {"disabledSupport": True, "resolveSupport": {"properties": ["edit"]}}, "rename": {"honorsChangeAnnotations": annotations}}
        workspace = {"applyEdit": True, "workspaceEdit": {"changeAnnotationSupport": {}}}
        options = {} if view is None else {"initializationOptions": {"view": view}}
        return self.request("initialize", {"capabilities": {"workspace": workspace, "textDocument": document}, **options})


def says(published, text):
    """Whether one of the diagnostics published mentions `text`."""
    return published is not None and any(text in each["message"] for each in published["diagnostics"])


def file_name(uri):
    return urllib.parse.unquote(uri).rsplit("/", 1)[1]


def edits(edit):
    """The changes of a WorkspaceEdit, each as its file's name, its version
    and its edits; None for none."""
    return edit and [(file_name(change["textDocument"]["uri"]), change["textDocument"]["version"], change["edits"]) for change in edit["documentChanges"]]


def text_edit(text, needle, new, length=None):
    """The edit of the first `length` characters of `needle` in `text`."""
    start = text.index(needle)
    return {"range": {"start": lsp_position(text, start), "end": lsp_position(text, start + (len(needle) if length is None else length))}, "newText": new}


def lines_changed(old, new):
    """An incremental change from `old` to `new`: the lines between those both
    begin and end with."""
    a, b = old.splitlines(keepends=True), new.splitlines(keepends=True)
    head = 0
    while head < min(len(a), len(b)) and a[head] == b[head]:
        head += 1
    tail = 0
    while tail < min(len(a), len(b)) - head and a[-1 - tail] == b[-1 - tail]:
        tail += 1
    return {"range": {"start": {"line": head, "character": 0}, "end": {"line": len(a) - tail, "character": 0}}, "text": "".join(b[head : len(b) - tail])}


def hold(project, editor, name, printed, content=""):
    """Asks the helper plugin through the file `name`, whole when the editor
    sees it, and waits for it to print `printed` once more; what it printed
    after that."""
    count = editor.output.count(printed) + 1
    (project / f"{name}.part").write_text(content, encoding="utf-8")
    (project / f"{name}.part").replace(project / name)
    editor.wait_for(printed, count)
    return editor.output.decode("utf-8", "replace").rsplit(printed.decode(), 1)[-1].split("\n", 1)[0].strip()


def run_lsp_group(godot):
    """The language server (architecture §2.4) in a headless editor on a copy
    of tests/engine/lsp, at a path with a space and CJK."""
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp).resolve() / "lsp 项目"
        shutil.copytree(LSP, project)
        # With CRLF, as a Windows checkout may write it; its tab in Godot has
        # no carriage returns.
        gunner = project / "gunner.pet"
        gunner.write_bytes(gunner.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        (project / "bin").mkdir()
        shutil.copy(PROJECT / host_library("editor"), project / host_library("editor"))
        shutil.copy(PROJECT / "petrichor.gdextension", project)
        info = project / ".godot" / "petrichor" / "lsp.json"
        import_project(godot, project)
        code, output = run(godot, "-e", "--path", str(project), "--", "--petrichor-build")
        expect("lsp: a headless editor serves only with --petrichor-lsp", "petrichor build:" in output and not info.exists(), output)
        editor = Running(godot, "-e", "--path", str(project), "--", "--petrichor-lsp", "--lsp-hold")
        try:
            check_lsp(project, info, editor)
        finally:
            (project / "quit").touch()
            code, output = editor.finish()
        # The engine's String prints its faults without "ERROR".
        errors = [line for line in output.splitlines() if "ERROR" in line or "Unicode parsing error" in line]
        expect("lsp: the editor quits without errors, and its lsp.json with it", code == 0 and not errors and not info.exists(), output)


PET = Path("bin", {"win32": "pet.windows.x86_64.exe", "linux": "pet.linux.x86_64", "darwin": "pet.macos.universal"}[sys.platform])


def lsp_reply(stream):
    """The body of the next message on the stream."""
    header = b""
    while not header.endswith(b"\r\n\r\n") and (byte := stream.read(1)):
        header += byte
    found = re.search(rb"Content-Length: (\d+)", header)
    return json.loads(stream.read(int(found.group(1)))) if found else None


def run_pet_group(godot):
    """pet (architecture §6.5) in a project's addons/petrichor/. Without a
    service it runs a headless editor, which imports a fresh copy first. With
    one it asks the service, which reads the files as they are, and refuses a
    service of another build."""
    if not (PROJECT / PET).is_file():
        expect("pet: it is built", False, f"{PROJECT / PET} is missing. Build it with: scons")
        return
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp).resolve() / "pet 项目"
        addon = project / "addons" / "petrichor"
        (addon / "bin").mkdir(parents=True)
        for name in ("petrichor.gdextension", host_library("editor"), PET):
            shutil.copy(PROJECT / name, addon / name)
        # A downloaded CI artifact loses it.
        (addon / PET).chmod(0o755)
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        script = project / "broken.pet"
        script.write_text('extends Node\n\n\nfunc _ready() -> void:\n\tvar x: int = "a"\n', encoding="utf-8")

        def pet(*args):
            ran = subprocess.run([addon / PET, *args], capture_output=True, timeout=TIMEOUT)
            return ran.returncode, ran.stdout.decode("utf-8", "replace"), ran.stderr.decode("utf-8", "replace")

        error = "broken.pet:5:15: error: Cannot assign a value of type String as int."
        summary = "petrichor check: 1 script(s), 0 scene(s), 0 resource file(s), {} with errors"
        code, out, err = pet("check", "--godot", godot)
        expect("pet: without a service, a headless editor checks a fresh copy", code == 1 and error in out.splitlines() and summary.format(1) in out.splitlines(), out + err)
        expect("pet: check writes no bytecode", not list(project.glob(".godot/**/*.petc")))

        info = project / ".godot" / "petrichor" / "lsp.json"
        editor = Running(godot, "-e", "--path", str(project), "--", "--petrichor-lsp")
        try:
            while not info.exists() and not editor.ended and editor._remaining() > 0:
                time.sleep(0.05)
            code, out, err = pet("check")
            expect("pet: a service checks, printing each error, and exits with 1", (code, out.splitlines()) == (1, [error, summary.format(1)]), out + err)
            started = info.read_text(encoding="utf-8")
            info.write_text(json.dumps({**json.loads(started), "build": "other"}), encoding="utf-8")
            code, out, err = pet("check")
            expect("pet: a service of another build is refused, with 2", code == 2 and "build other" in err, out + err)
            info.write_text(started, encoding="utf-8")
            script.write_text("extends Node\n\n\nfunc _ready() -> void:\n\tvar x: int=1\n", encoding="utf-8")
            code, out, err = pet("format")
            expect("pet: a service formats the files as they are now", code == 0 and "var x: int = 1" in script.read_text(encoding="utf-8"), out + err)
            code, out, err = pet("check")
            expect("pet: a project without errors gives 0", (code, out.splitlines()) == (0, [summary.format(0)]), out + err)
            bridge = subprocess.Popen([addon / PET, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}}).encode()
            bridge.stdin.write(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))
            bridge.stdin.flush()
            reply = lsp_reply(bridge.stdout)
            bridge.stdin.close()
            expect("pet: lsp bridges a client to the service until stdin closes", reply and "capabilities" in reply.get("result", {}) and bridge.wait(timeout=10) == 0, str(reply))
        finally:
            editor.process.kill()
            code, output = editor.finish()
            # On Windows the editor holds its copy of the library until the
            # console wrapper's job has ended it.
            copy = addon / "bin" / ("~" + Path(host_library("editor")).name)
            deadline = time.monotonic() + 10
            while copy.exists() and time.monotonic() < deadline:
                try:
                    copy.unlink()
                except OSError:
                    time.sleep(0.1)
        expect("pet: the service reports no errors", "ERROR" not in output, output)


FOLDS_CASE = sys.platform in ("win32", "darwin")


def short_path(path):
    """An existing path in Windows' 8.3 names, where the volume makes them."""
    if sys.platform != "win32":
        return path
    import ctypes

    buffer = ctypes.create_unicode_buffer(32768)
    size = ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer))
    return Path(buffer.value) if 0 < size < len(buffer) else path


def outside_uris(project, uri):
    """URIs the server takes no file of: outside the project, not a script,
    under .godot/ in any case where names fold case, and not a file."""
    return [file_uri(project.parent / "outside.pet"), uri("notes.txt"), uri(".godot/x.pet"), *([uri(".GODOT/x.pet")] if FOLDS_CASE else []), "untitled:Untitled-1"]


def check_lsp(project, info, editor):
    deadline = time.monotonic() + TIMEOUT
    while not info.exists() and not editor.ended and time.monotonic() < deadline:
        time.sleep(0.1)
    server = json.loads(info.read_text(encoding="utf-8")) if info.exists() else {}
    expect("lsp: the editor writes its process and port", server.get("pid", 0) > 0 and server.get("port") not in (None, 6005, 6008), str(server))
    # Once the first scan is done and the refactoring written, which the
    # server takes in at once (PetEditorPlugin::changed()).
    editor.wait_for(b"lsp hold: done")
    if not server:
        return

    def uri(name, encoded=True):
        return file_uri(project / name, encoded)

    # A connection that never initializes is let go, which the end looks for.
    silent = LspClient(server["port"])
    first = LspClient(server["port"])
    early = first.take(lambda m: True, 0.5)
    hover = first.request("textDocument/hover", {})
    first.open(uri("board.pet"), "x")
    expect("lsp: before initialize, nothing is sent and requests are refused", early is None and hover and hover["error"]["code"] == -32002, str(hover))
    reply = first.initialize()
    capabilities = reply and reply["result"]["capabilities"]
    expect("lsp: initialize offers incremental sync in UTF-16", capabilities and capabilities["positionEncoding"] == "utf-16" and capabilities["textDocumentSync"]["change"] == 2, str(reply))
    first.notify("initialized", {})

    # Answered or passed over, and the messages after them read (clangd's protocol.test).
    first.send_bytes(b"Content-Length: 5\r\n\r\n{bad}")
    bad_json = first.take(lambda m: "error" in m)
    first.send_bytes(b"Content-Length: 2\r\n\r\n\xff\xfe")
    bad_utf8 = first.take(lambda m: "error" in m)
    first.send_bytes(b"Content-Type: text\r\n\r\n{}Content-Length: 99999999999999\r\n\r\n")
    first.open(uri("rank.pet"), 5)
    first.notify("textDocument/didChange", {"textDocument": {"uri": 7}, "contentChanges": "none"})
    for outside in outside_uris(project, uri):
        first.open(outside, "x")
    # Responses are not answered, also without an id (JSON-RPC 2.0).
    first.send({"id": None, "error": {"code": -32700, "message": "a response"}})
    first.send({"result": None})
    body = json.dumps({"jsonrpc": "2.0", "id": 90, "method": "petrichor/unknown"}).encode()
    first.send_bytes(b"Content-Length: %d\r\nX-Note: no content-length: 1\r\n\r\n%s" % (len(body), body))
    noted = first.take(lambda m: m.get("id") == 90)
    over = (16 << 20) + 1
    first.send_bytes(b"Content-Length: %d\r\n\r\n%s" % (over, b" " * over))
    skipped = first.take(lambda m: m.get("method") == "window/showMessage")
    unknown = first.request("petrichor/unknown")
    expect(
        "lsp: malformed messages are answered or passed over, a header by its lines, one too long told",
        bad_json and bad_json["error"]["code"] == -32700 and bad_json["id"] is None and bad_utf8 and bad_utf8["error"]["code"] == -32700
        and noted and noted["error"]["code"] == -32601 and skipped and str(over) in skipped["params"]["message"]
        and unknown and unknown["error"]["code"] == -32601,
        f"{bad_json}\n{bad_utf8}\n{noted}\n{skipped}\n{unknown}",
    )

    # A NUL is one character, which the engine's String does not take.
    nul = uri("nul.pet")
    nul_text = 'extends Node\n\nfunc f() -> void:\n\tprint("\x00\x00", u_nul)\n'
    first.open(nul, nul_text)
    published = first.diagnostics(nul, lambda p: says(p, "u_nul"))
    at = [d["range"]["start"] for d in published["diagnostics"] if "u_nul" in d["message"]] if published else []
    first.notify("textDocument/didClose", {"textDocument": {"uri": nul}})
    expect("lsp: a NUL in a text takes its place in columns", at == [lsp_position(nul_text, nul_text.index("u_nul"))], str(published))

    enemy = uri("enemy.pet")
    first.open(enemy, (project / "enemy.pet").read_text(encoding="utf-8"))
    published = first.diagnostics(enemy, lambda p: says(p, "max_speed"))
    expect("lsp: a script opened gets its errors and its scenes'", says(published, "max_speed") and says(published, "Head/Camera") and published["version"] == 1, str(published))

    player = uri("player.pet")
    first.open(player, (project / "player.pet").read_text(encoding="utf-8") + "pub var max_speed := 7.0\n")
    published = first.diagnostics(enemy, lambda p: not says(p, "max_speed"))
    expect("lsp: other scripts read the client's unsaved text", published and not says(published, "max_speed") and says(published, "Head/Camera"), str(published))
    first.notify("textDocument/didClose", {"textDocument": {"uri": player}})
    cleared = first.diagnostics(player, lambda p: "version" not in p)
    published = first.diagnostics(enemy, lambda p: says(p, "max_speed"))
    expect("lsp: a script closed goes back to its file, without diagnostics", cleared and cleared["diagnostics"] == [] and says(published, "max_speed"), f"{cleared}\n{published}")

    # One file under two spellings of its URI, the second in another case
    # where names fold case: one text, diagnostics for each, open until the
    # last is closed; and the engine never gets the other case.
    spelled = uri("PLAYER.pet") if FOLDS_CASE else uri("player.pet", encoded=False)
    faster = (project / "player.pet").read_text(encoding="utf-8") + "pub var max_speed := 7.0\n"
    first.open(player, faster)
    first.open(spelled, faster)
    both = [first.diagnostics(each, lambda p: True) for each in (player, spelled)]
    published = first.diagnostics(enemy, lambda p: not says(p, "max_speed"))
    first.notify("textDocument/didSave", {"textDocument": {"uri": spelled}})
    first.notify("textDocument/didClose", {"textDocument": {"uri": spelled}})
    first.notify("textDocument/didChange", {"textDocument": {"uri": player, "version": 2}, "contentChanges": [{"text": faster + "\nfunc g() -> int:\n\treturn u_spelled\n"}]})
    changed = first.diagnostics(player, lambda p: says(p, "u_spelled"))
    first.notify("textDocument/didClose", {"textDocument": {"uri": player}})
    cache = project / ".godot" / "uid_cache.bin"
    expect(
        "lsp: a file under two spellings of its URI is one script, open until the last is closed",
        all(both) and published and not says(published, "max_speed") and changed and changed["version"] == 2 and b"PLAYER" not in (cache.read_bytes() if cache.exists() else b""),
        f"{both}\n{published}\n{changed}",
    )

    # The project's path in Windows' 8.3 names, which a client may have as
    # well as the long ones that the editor has.
    short = short_path(project)
    if "~" not in str(short):
        say("lsp: skipped the 8.3 names of the project's path, as the volume makes none")
    else:
        short_enemy = file_uri(short / "enemy.pet")
        enemy_text = (project / "enemy.pet").read_text(encoding="utf-8")
        first.open(short_enemy, enemy_text)
        published = first.diagnostics(short_enemy, lambda p: True)
        ready = first.request("textDocument/prepareRename", {"textDocument": {"uri": short_enemy}, "position": lsp_position(enemy_text, enemy_text.index("chase"))})
        first.notify("textDocument/didClose", {"textDocument": {"uri": short_enemy}})
        expect("lsp: a script through the 8.3 names of the project's path is the project's", says(published, "max_speed") and ready and (ready.get("result") or {}).get("placeholder") == "chase", f"{short_enemy}\n{published}\n{ready}")

    # Positions in UTF-16 after CJK and an emoji, in a CRLF text changed in
    # steps: within a line, across lines, and one past the line or the text.
    wide = uri("wide.pet", encoded=False)
    text = 'extends Node\r\n\r\nfunc f() -> void:\r\n\tprint("雨😀", missing)\r\n'
    first.open(wide, text)
    published = first.diagnostics(wide, lambda p: says(p, "missing"))
    at = [d["range"]["start"] for d in published["diagnostics"] if "missing" in d["message"]] if published else []
    expect("lsp: columns count UTF-16 code units", at == [{"line": 3, "character": 14}], str(published))

    def changed(version, *changes):
        nonlocal text
        sent = []
        for start, end, new in changes:
            sent.append({"range": {"start": start, "end": end}, "text": new})
            begin, finish = sorted((lsp_index(text, start), lsp_index(text, end)))
            text = text[:begin] + new + text[finish:]
        first.notify("textDocument/didChange", {"textDocument": {"uri": wide, "version": version}, "contentChanges": sent})

    def place(name):
        return lsp_position(text, text.index(name))

    # Each change after the first is before it in the text, so that its
    # place is the same before and after the first.
    changed(2, (place("missing"), place("missing"), '"雪🌧" + '), ({"line": 0, "character": 12}, {"line": 1, "character": 0}, ""))
    changed(3, (place("missing"), place(")\r\n"), "missing2"), ({"line": 1, "character": 17}, {"line": 1, "character": 17}, "\r\n\tvar 雪 := 1"))
    changed(4, ({"line": 99, "character": 0}, {"line": 99, "character": 5}, "\r\n# end"), ({"line": 0, "character": 999}, {"line": 0, "character": -4}, ""))
    versions = []
    published = first.diagnostics(wide, lambda p: versions.append(p["version"]) or says(p, "missing2"))
    at = [d["range"]["start"] for d in published["diagnostics"] if "missing2" in d["message"]] if published else []
    expect("lsp: changes apply in order, and diagnostics carry the version they are for", at == [place("missing2")] and versions[-1:] == [4] and 2 not in versions and 3 not in versions, f"{versions}\n{published}\n{text!r}")

    # While user.pet has an unsaved change in Godot, which a quick fix and a rename are refused for.
    check_queries(first, project, uri)
    check_rename(first, project, uri)

    # Architecture §2.3: a refactoring in Godot writes the scripts it changes,
    # so a client that opens one after it is sent nothing.
    draft = uri("draft.pet")
    draft_text = (project / "draft.pet").read_text(encoding="utf-8")
    first.open(draft, draft_text)
    pushed = first.take(lambda m: m.get("method") == "workspace/applyEdit", 1)
    first.notify("textDocument/didClose", {"textDocument": {"uri": draft}})
    expect("lsp: a script that a refactoring in Godot changed is opened as written, and nothing is pushed", "oranges" in draft_text and pushed is None, f"{draft_text}\n{pushed}")

    user = uri("user.pet")
    user_text = (project / "user.pet").read_text(encoding="utf-8")
    first.open(user, user_text)

    # A member renamed from user.pet, open in the client, in the scripts and a
    # text scene; the words in the helper plugin's GDScript are candidates.
    texts = {name: (project / name).read_text(encoding="utf-8") for name in ["hit.pet", "level.tscn", "addons/lsp_hold/plugin.gd"]}
    words = [lsp_position(texts["addons/lsp_hold/plugin.gd"], m.start()) for m in re.finditer(r"\bdamage\b", texts["addons/lsp_hold/plugin.gd"])]
    renamed = first.request("textDocument/rename", {"textDocument": {"uri": user}, "position": lsp_position(user_text, user_text.index("damage")), "newName": "power"})
    renamed = renamed and renamed.get("result")
    expect(
        "lsp: a rename is one edit of scripts and text scenes, its candidates to confirm",
        renamed
        and edits(renamed)
        == [
            ("plugin.gd", None, [{"range": {"start": at, "end": {"line": at["line"], "character": at["character"] + 6}}, "newText": "power", "annotationId": "candidate"} for at in words]),
            ("hit.pet", None, [text_edit(texts["hit.pet"], "damage", "power")]),
            ("level.tscn", None, [text_edit(texts["level.tscn"], "damage", "power")]),
            ("user.pet", 1, [text_edit(user_text, "damage", "power")]),
        ]
        and renamed["changeAnnotations"]["candidate"]["needsConfirmation"] is True,
        str(renamed),
    )

    # A scene saved reaches the scripts it places at once.
    level = project / "level.tscn"
    level.write_text(texts["level.tscn"] + '\n[node name="Head" type="Node3D" parent="."]\n\n[node name="Camera" type="Camera3D" parent="Head"]\n', encoding="utf-8")
    first.notify("textDocument/didSave", {"textDocument": {"uri": uri("level.tscn")}})
    published = first.diagnostics(enemy, lambda p: not says(p, "Head/Camera"))
    expect("lsp: a scene saved reaches the scripts it places at once", published and not says(published, "Head/Camera") and says(published, "max_speed"), str(published))
    check_rename_written(first, project, uri, editor)
    check_closed_unsaved(first, project, uri, editor)
    check_scene_saved_in_godot(first, project, uri, editor)
    check_resource_saved_with_scene(first, project, uri, editor)
    check_rename_crlf(first, project, uri, editor)
    check_refactoring_client_open(first, project, uri, editor)
    check_rename_in_godot_client_open(first, project, uri, editor)
    check_conflicts(first, project, editor, server["port"])
    first.notify("textDocument/didClose", {"textDocument": {"uri": user}})
    check_sessions(project, uri, server["port"])

    # A save writes the bytecode of the file as the files compile, not as an
    # unsaved dependency in the client would make it.
    stat = uri("stat.pet")
    first.open(stat, "class_name Stat extends RefCounted\n\npub const L := 99\n")
    bytecode = project / ".godot" / "petrichor" / "score.petc"
    bytecode.unlink(missing_ok=True)
    score = project / "score.pet"
    score.write_bytes(score.read_bytes())
    first.notify("textDocument/didSave", {"textDocument": {"uri": uri("score.pet")}})
    first.request("textDocument/documentSymbol", {"textDocument": {"uri": stat}})
    first.notify("textDocument/didClose", {"textDocument": {"uri": stat}})
    expect("lsp: a save writes the bytecode of the file as the files on disk compile it", bytecode.exists(), str(bytecode))

    # A file changed and a file added on disk, with no notification, reach the
    # editor's class list and the human's view within seconds (architecture
    # §6.5; godotengine/godot#42786).
    board = uri("board.pet")
    first.open(board, (project / "board.pet").read_text(encoding="utf-8"))
    published = first.diagnostics(board, lambda p: says(p, "Rank") and says(p, "Bonus"))
    (project / "rank.pet").write_text("class_name Rank extends RefCounted\n\npub const TOP := 3\n", encoding="utf-8")
    (project / "bonus.pet").write_text("class_name Bonus extends RefCounted\n\npub const VALUE := 2\n", encoding="utf-8")
    found = first.diagnostics(board, lambda p: not p["diagnostics"], 5)
    expect("lsp: a file changed and a file added on disk, unannounced, reach the other scripts within seconds", says(published, "Rank") and says(published, "Bonus") and found and not found["diagnostics"], f"{published}\n{found}")

    outside = outside_uris(project, uri)
    stray = [m for m in first.kept if "error" in m or (m.get("params") or {}).get("uri") in outside]
    expect("lsp: files outside the project, other files and malformed notifications get nothing", not stray, str(stray))

    first.socket.close()
    second = LspClient(server["port"])
    second.initialize(annotations=False)
    hit = texts["hit.pet"]
    renamed = second.request("textDocument/rename", {"textDocument": {"uri": uri("hit.pet")}, "position": lsp_position(hit, hit.index("damage")), "newName": "power"})
    renamed = renamed and renamed.get("result")
    expect("lsp: a client that cannot mark candidates does not get them", renamed and [name for name, _, _ in edits(renamed)] == ["hit.pet", "level.tscn", "user.pet"] and "changeAnnotations" not in renamed, str(renamed))
    expect("lsp: a client that does not initialize is let go", silent.closed(15), "")
    down = second.request("shutdown")
    after = second.request("petrichor/unknown")
    second.notify("exit", None)
    expect("lsp: shutdown, then exit closes the connection", down and down["result"] is None and after and after["error"]["code"] == -32600 and second.closed(), f"{down}\n{after}")


def check_queries(client, project, uri):
    """The queries on tour.pet, which tour.tscn places and tour.scn, a binary
    copy, too. What the compiler answers the editor tests check; these check
    what the server makes of it: places, kinds, ranges, URIs and versions."""
    tour = uri("tour.pet")
    texts = {name: (project / name).read_text(encoding="utf-8") for name in ["tour.pet", "player.pet", "tour.tscn"]}
    text = texts["tour.pet"]
    client.open(tour, text)

    def span(needle, length=None, plus=0, name="tour.pet"):
        start = texts[name].index(needle) + plus
        return {"start": lsp_position(texts[name], start), "end": lsp_position(texts[name], start + (len(needle) if length is None else length))}

    def ask(method, needle, plus=0, **params):
        reply = client.request(method, {"textDocument": {"uri": tour}, "position": span(needle, 0, plus)["start"], **params})
        return reply and reply.get("result")

    def located(location):
        return file_name(location["uri"]), location["range"]

    listed = ask("textDocument/completion", "speed, player.armor", 2, context={"triggerKind": 1})
    items = {item["label"]: item for item in listed["items"]} if listed else {}
    paths = ask("textDocument/completion", "Camera.rotate", 2, context={"triggerKind": 2, "triggerCharacter": "a"})
    files = ask("textDocument/completion", 'user.pet")', 2, context={"triggerKind": 1})
    after_parenthesis = ask("textDocument/completion", "maxf(", 5, context={"triggerKind": 2, "triggerCharacter": "("})
    expect(
        "lsp: completion replaces what the editor does, a call as a snippet, a path in a string quoted",
        items.get("speed", {}).get("kind") == 10
        and items["speed"]["textEdit"] == {"range": span("speed, player.armor", 2), "newText": "speed"}
        and items.get("add_child", {}).get("textEdit", {}).get("newText") == "add_child($1)"
        and items["add_child"].get("insertTextFormat") == 2
        and {"label": "Head/Camera", "range": span("Head/Ca"), "newText": "Head/Camera"} in [{"label": i["label"], **i["textEdit"]} for i in paths["items"]]
        and {"range": span('"us'), "newText": '"res://user.pet"'} in [i["textEdit"] for i in files["items"]]
        and after_parenthesis["items"] == [],
        f"{items.get('speed')}\n{items.get('add_child')}\n{paths}\n{files and files['items'][:3]}\n{after_parenthesis}",
    )

    signature = ask("textDocument/signatureHelp", "2.0)")
    expect("lsp: signature help shows the innermost call with the parameter at the cursor", signature == {"signatures": [{"label": "maxf(a: float, b: float) -> float", "parameters": [{"label": [15, 23]}], "activeParameter": 0}], "activeSignature": 0}, str(signature))

    shown = [hover and hover["contents"]["value"] for hover in (ask("textDocument/hover", needle) for needle in ["雨,", "move(maxf", "speed = maxf", "rotate_y"])]
    expect(
        "lsp: hover shows the call hint's signature or the type, and the docs",
        shown == ["```petrichor\nvar 雨: String\n```", "```petrichor\nfunc Tour.move(amount: float, player: Player) -> void\n```\n\nMoves by [param amount].", "```petrichor\nvar Tour.speed: float\n```", "```petrichor\nfunc Node3D.rotate_y(angle: float) -> void\n```"],
        str(shown),
    )

    speed = ask("textDocument/definition", "speed, player.armor")
    camera = ask("textDocument/definition", "Camera.rotate")
    expect(
        "lsp: definition goes to the name, not into the engine, and from a node path to its node in the scene",
        speed and located(speed) == ("player.pet", span("speed", name="player.pet")) and ask("textDocument/definition", "rotate_y") is None and camera and [located(each) for each in camera] == [("tour.tscn", span('[node name="Camera"', 0, name="tour.tscn"))],
        f"{speed}\n{camera}",
    )

    types = [[located(each) for each in ask("textDocument/typeDefinition", needle) or []] for needle in ["player.speed", "shots:\n"]]
    expect("lsp: type definition goes to the class and to the struct an array holds", types == [[("player.pet", span("Player", name="player.pet"))], [("tour.pet", span("Shot"))]], str(types))

    found = sorted((located(each) for each in ask("textDocument/references", "speed := 5.0", context={"includeDeclaration": False}) or []), key=str)
    uses = [("tour.pet", span(needle, 5)) for needle in ["speed, 2.0", "speed = maxf", "speed, 1.0", "speed:\n"]]
    start = {"line": 0, "character": 0}
    stored = [("tour.tscn", span("speed", name="tour.tscn")), ("tour.scn", {"start": start, "end": start})]
    expect("lsp: references reach the line a text scene stores the name at and the start of a binary one, without the declaration", found == sorted(uses + stored, key=str), str(found))

    symbols = client.request("textDocument/documentSymbol", {"textDocument": {"uri": tour}})["result"]

    def tree(symbol):
        return [symbol["name"], symbol["kind"], [tree(child) for child in symbol["children"]]]

    move = symbols[0]["children"][-2] if symbols else {}
    expect(
        "lsp: the outline nests the class's members, an enum's values and a struct's fields",
        [tree(each) for each in symbols] == [["Tour", 5, [["User", 14, []], ["hit", 24, []], ["State", 10, [["IDLE", 22, []], ["RUN", 22, []]]], ["Shot", 23, [["damage", 8, []]]], ["speed", 8, []], ["shots", 8, []], ["move", 6, []], ["count", 6, []]]]]
        and move.get("range") == {"start": span("func move", 0)["start"], "end": span("pass\n\n\nfunc", 4)["end"]}
        and move.get("selectionRange") == span("move(amount", 4),
        str(symbols),
    )

    member = [each["range"] for each in ask("textDocument/documentHighlight", "speed = maxf") or []]
    local = [each["range"] for each in ask("textDocument/documentHighlight", "speed\n") or []]
    expect(
        "lsp: highlights are the names of one declaration, a local apart from the member it shadows",
        member == [span(needle, 5) for needle in ["speed := 5.0", "speed, 2.0", "speed = maxf", "speed, 1.0", "speed:\n"]] and local == [span(needle, 5) for needle in ["speed := 2", "speed\n"]],
        f"{member}\n{local}",
    )

    def actions(needle):
        return client.request("textDocument/codeAction", {"textDocument": {"uri": tour}, "range": span(needle, 0), "context": {"diagnostics": []}})["result"]

    def resolved(action):
        reply = client.request("codeAction/resolve", action)
        return (reply and reply.get("result") or {}).get("edit")

    truth = actions("shots:\n")
    pub = actions("User.new().f")
    test = {"range": span("shots:\n", 5), "newText": "not shots.is_empty()"}
    expect(
        "lsp: quick fixes come as the editor lists them, the automatic one preferred, each edit resolved when chosen",
        [(each["title"], each.get("isPreferred"), "edit" in each) for each in truth] == [("Write not shots.is_empty()", True, False), ("Fix all 'Write the truth test' in file", None, False)]
        and truth[0]["diagnostics"][0]["range"] == test["range"]
        and edits(resolved(truth[0])) == [("tour.pet", 1, [test])]
        and edits(resolved(truth[1])) == [("tour.pet", 1, [test, {"range": span("speed:\n", 5), "newText": "speed != 0"}])]
        and edits(resolved(pub[1])) == [("player.pet", None, [{"range": span("var armor", name="player.pet"), "newText": "pub var armor"}])],
        f"{truth}\n{pub}",
    )

    # VS Code shows no disabled action among the quick fixes, nor its reason.
    refused = resolved(pub[0])
    told = client.take(lambda m: m.get("method") == "window/showMessage")
    expect(
        "lsp: a fix refused for a script Godot has unsaved says so in its title, and choosing it edits nothing and tells why",
        [(each["title"], "disabled" in each) for each in pub] == [('Make "f" pub (save user.pet in Godot first)', False), ('Make "armor" pub', False)] and refused is None and told and "res://user.pet" in told["params"]["message"],
        f"{pub}\n{refused}\n{told}",
    )

    # VS Code applies a resolved edit without checking the version it names.
    client.notify("textDocument/didChange", {"textDocument": {"uri": tour, "version": 2}, "contentChanges": [{"range": {"start": start, "end": start}, "text": "\n"}]})
    moved = resolved(truth[0])
    told = client.take(lambda m: m.get("method") == "window/showMessage")
    client.notify("textDocument/didChange", {"textDocument": {"uri": tour, "version": 3}, "contentChanges": [{"text": text + "# The end.\n"}]})
    kept = resolved(truth[0])
    expect(
        "lsp: a fix chosen after its text changed is found again, at the offsets it was listed at, or refused with a message",
        moved is None and told and "changed" in told["params"]["message"] and kept and edits(kept) == [("tour.pet", 3, [test])],
        f"{moved}\n{told}\n{kept}",
    )

    messy = text.replace("var shots", "var  shots").replace("雨 :=", "雨  :=").replace("move(amount", "move( amount")
    client.notify("textDocument/didChange", {"textDocument": {"uri": tour, "version": 4}, "contentChanges": [{"text": messy}]})
    formatted = messy
    for edit in reversed(client.request("textDocument/formatting", {"textDocument": {"uri": tour}, "options": {"tabSize": 4, "insertSpaces": False}})["result"] or []):
        formatted = formatted[: lsp_index(formatted, edit["range"]["start"])] + edit["newText"] + formatted[lsp_index(formatted, edit["range"]["end"]) :]
    client.notify("textDocument/didChange", {"textDocument": {"uri": tour, "version": 5}, "contentChanges": [{"text": "func ("}]})
    broken = client.request("textDocument/formatting", {"textDocument": {"uri": tour}, "options": {}})
    client.notify("textDocument/didClose", {"textDocument": {"uri": tour}})
    expect("lsp: formatting edits make the editor's layout, and a syntax error fails it", formatted == text and broken and broken["error"]["code"] == -32803, f"{formatted!r}\n{broken}")


def check_rename(client, project, uri):
    """Renaming from tour.pet while user.pet has an unsaved change in Godot: where
    prepareRename gives the name or the editor's reason to refuse it, a local
    in one edit, and the reasons a rename is refused, each readable."""
    tour = uri("tour.pet")
    text = (project / "tour.pet").read_text(encoding="utf-8")
    client.open(tour, text)
    ready = uri("ready.pet")
    client.open(ready, "extends Node\n\n\nfunc _ready() -> void:\n\tpass\n")

    def ask(method, needle, plus=0, **params):
        return client.request(method, {"textDocument": {"uri": tour}, "position": lsp_position(text, text.index(needle) + plus), **params})

    def reason(reply):
        return reply and reply.get("error", {}).get("message")

    local = ask("textDocument/prepareRename", "speed := 2", 2)
    refused = [reason(ask("textDocument/prepareRename", needle)) for needle in ["rotate_y", "Camera.rotate"]]
    override = reason(client.request("textDocument/prepareRename", {"textDocument": {"uri": ready}, "position": {"line": 3, "character": 6}}))
    client.notify("textDocument/didClose", {"textDocument": {"uri": ready}})
    expect(
        "lsp: prepareRename gives the name, or the editor's reason not to rename it",
        local
        and local.get("result") == {"range": text_edit(text, "speed := 2", "", 5)["range"], "placeholder": "speed"}
        and refused == ["It is not declared in this language.", "A node is renamed in the scene editor."]
        and override == "res://ready.pet:4: It overrides a method that the engine or another language calls by this name.",
        f"{local}\n{refused}\n{override}",
    )

    renamed = ask("textDocument/rename", "speed := 2", newName="pace")
    changes = [text_edit(text, "speed := 2", "pace", 5), text_edit(text, "speed\n", "pace", 5)]
    expect("lsp: a local is renamed in one versioned edit of its script", renamed and renamed.get("result") == {"documentChanges": [{"textDocument": {"uri": tour, "version": 1}, "edits": changes}]}, str(renamed))

    # Refused: by the compiler, each reason with its place unless it is the
    # script's own; and for user.pet, unsaved in Godot.
    answers = [reason(ask("textDocument/rename", needle, newName=new)) for needle, new in [("speed := 5.0", "pace"), ("move(amount", "count"), ("move(amount", "2x"), ("f)\n", "g")]]
    client.notify("textDocument/didClose", {"textDocument": {"uri": tour}})
    expect(
        "lsp: a rename refused tells each reason where it is, and to save what Godot has unsaved",
        answers[0] == "res://tour.scn: It is a binary file, whose names cannot be rewritten. Save it as .tscn first."
        and answers[1] and answers[1].startswith("res://tour.pet:") and "After renaming" in answers[1]
        and answers[2] == '"2x" is not a name.'
        and answers[3] == "Godot has unsaved changes to res://user.pet. Save them in Godot first.",
        str(answers),
    )


def check_rename_written(client, project, uri, editor):
    """A rename the client writes, the resource saved seconds before the
    scripts: Godot's loaded blaster.tres holds the value under the new
    name, and saving arena.tscn,
    open in Godot with gunner.pet in a tab, keeps its defaults, its
    connection and the resource's value."""
    weapon = uri("weapon.pet")
    texts = {name: (project / name).read_text(encoding="utf-8") for name in ["weapon.pet", "gunner.pet", "blaster.tres"]}
    client.open(weapon, texts["weapon.pet"])

    # A member that a resource or a node leaves at its default is stored
    # nowhere, so neither refers to it.
    shots = client.request("textDocument/rename", {"textDocument": {"uri": weapon}, "position": lsp_position(texts["weapon.pet"], texts["weapon.pet"].index("shots")), "newName": "rounds"})
    speed = client.request("textDocument/references", {"textDocument": {"uri": uri("gunner.pet")}, "position": lsp_position(texts["gunner.pet"], texts["gunner.pet"].index("movement_speed")), "context": {"includeDeclaration": False}})
    expect(
        "lsp: a member a resource or a node leaves at its default is no reference there",
        shots and "result" in shots and [name for name, _, _ in edits(shots["result"])] == ["weapon.pet"] and speed and speed.get("result") == [],
        f"{shots}\n{speed}",
    )

    renamed = client.request("textDocument/rename", {"textDocument": {"uri": weapon}, "position": lsp_position(texts["weapon.pet"], texts["weapon.pet"].index("spread")), "newName": "shot_spread"})
    changes = [change for change in edits(renamed["result"]) if change[0] in texts] if renamed and "result" in renamed else []
    # The client applies the edit to the scripts' buffers, and the user saves
    # the resource first and the scripts seconds later, as with VS Code's
    # refactoring auto-save off.
    written = {}
    for name, _, file_edits in changes:
        text = texts[name]
        for edit in reversed(file_edits):
            text = text[: lsp_index(text, edit["range"]["start"])] + edit["newText"] + text[lsp_index(text, edit["range"]["end"]) :]
        written[name] = text
    gunner = uri("gunner.pet")
    if changes:
        client.notify("textDocument/didChange", {"textDocument": {"uri": weapon, "version": 2}, "contentChanges": [{"text": written["weapon.pet"]}]})
        client.open(gunner, written["gunner.pet"])
        # Taken in before the resource is saved, which is seconds later in use.
        client.request("petrichor/unknown")
        (project / "blaster.tres").write_text(written["blaster.tres"], encoding="utf-8", newline="")
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri("blaster.tres"), "type": 2}]})
        time.sleep(3)
        for name in ["weapon.pet", "gunner.pet"]:
            (project / name).write_text(written[name], encoding="utf-8", newline="")
            client.notify("textDocument/didSave", {"textDocument": {"uri": uri(name)}})
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(name), "type": 2} for name in ["weapon.pet", "gunner.pet"]]})
    for document in [weapon, gunner]:
        client.notify("textDocument/didClose", {"textDocument": {"uri": document}})
    time.sleep(1)
    (project / "check").touch()
    saved = editor.wait_for(b"lsp check: saved")
    held = re.findall(r"lsp check: (\S+) (\S+)", editor.output.decode("utf-8", "replace"))
    arena = (project / "arena.tscn").read_text(encoding="utf-8")
    blaster = (project / "blaster.tres").read_text(encoding="utf-8")
    expect(
        "lsp: a rename the client writes reaches what Godot has loaded, so that saving there keeps it",
        [name for name, _, _ in changes] == ["blaster.tres", "gunner.pet", "weapon.pet"]
        and saved
        and held == [("1.0", "<null>")]
        and "shot_spread = 1.0" in blaster
        and "null" not in arena
        and 'signal="health_updated"' in arena,
        f"{renamed}\n{held}\n{arena}\n{blaster}",
    )

    # A refactoring in Godot, its edits computed on the client's text, of a
    # script whose tab has another text changes nothing.
    other = "# From the client.\n" + (project / "gunner.pet").read_text(encoding="utf-8")
    client.open(gunner, other)
    start = len(other[: other.index("var weapon") + 4].encode())
    (project / "refactor").write_text(json.dumps(["res://gunner.pet", start, start + 6, "weapon", "gun"]), encoding="utf-8")
    refactored = editor.wait_for(b"lsp refactor:")
    tab = re.findall(r"lsp refactor: (.*)", editor.output.decode("utf-8", "replace"))
    client.notify("textDocument/didClose", {"textDocument": {"uri": gunner}})
    expect("lsp: a refactoring in Godot of a script whose tab differs from the client's text changes nothing", refactored and len(tab) == 1 and "var weapon" in tab[0] and "movement_speed" in tab[0] and "gun" not in tab[0], str(tab))


def check_closed_unsaved(client, project, uri, editor):
    """A resource the client wrote gives Godot's loaded weapon.pet the
    client's unsaved text; closed without saving, the script goes back to
    its file, as a tab whose changes are discarded."""
    weapon = uri("weapon.pet")
    text = (project / "weapon.pet").read_text(encoding="utf-8")
    client.open(weapon, text)
    client.notify("textDocument/didChange", {"textDocument": {"uri": weapon, "version": 2}, "contentChanges": [{"text": text.replace("@export pub var shots := 1\n", "")}]})
    blaster = project / "blaster.tres"
    blaster.write_bytes(blaster.read_bytes() + b"\n")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri("blaster.tres"), "type": 2}]})
    client.notify("textDocument/didClose", {"textDocument": {"uri": weapon}})
    # Answered once the notifications before it are handled.
    client.request("petrichor/unknown")
    # Whole when the editor sees it.
    (project / "probe.part").write_text("res://weapon.pet", encoding="utf-8")
    (project / "probe.part").replace(project / "probe")
    editor.wait_for(b"lsp probe:")
    probed = re.findall(r"lsp probe: (\S+) (\S*)", editor.output.decode("utf-8", "replace"))
    expect("lsp: a script whose unsaved text Godot took goes back to its file once the client closes it", len(probed) == 1 and probed[0][0] == "true" and "shots" in probed[0][1].split(","), str(probed))


def check_scene_saved_in_godot(client, project, uri, editor):
    """A scene saved in Godot keeps its history to undo when the client's
    file watcher reports the save."""
    (project / "undo").touch()
    saved = editor.wait_for(b"lsp undo: saved")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri("arena.tscn"), "type": 2}]})
    client.request("petrichor/unknown")
    (project / "history").touch()
    editor.wait_for(b"lsp undo: kept")
    undo = re.findall(r"lsp undo: (\w+ \w+)", editor.output.decode("utf-8", "replace"))
    expect("lsp: a scene saved in Godot keeps its history when the client's file watcher reports it", saved and undo == ["saved true", "kept true"], str(undo))


def check_resource_saved_with_scene(client, project, uri, editor):
    """A resource edited in Godot, which saves it with the scene and reports
    only the scene, is not read back: it keeps its sub-resources, which its
    undo history and the inspector hold."""
    (project / "ramp").touch()
    saved = editor.wait_for(b"lsp ramp: saved")
    # The comparison at once, and answered once it is done.
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri("ramp.tres"), "type": 2}]})
    client.request("petrichor/unknown")
    (project / "ramp").touch()
    editor.wait_for(b"lsp ramp: kept")
    ramp = re.findall(r"lsp ramp: (\w+ \w+)", editor.output.decode("utf-8", "replace"))
    expect("lsp: a resource Godot saves with a scene is not read back, and keeps its sub-resources", saved and ramp == ["saved true", "kept true"], str(ramp))


def check_rename_crlf(client, project, uri, editor):
    """A rename in Godot's tab of gunner.pet, which the client has open with
    the carriage returns that the tab drops: the name at the caret, lines
    below the first, is renamed in place."""
    gunner = uri("gunner.pet")
    text = (project / "gunner.pet").read_text(encoding="utf-8")
    client.open(gunner, text.replace("\n", "\r\n"))
    client.request("petrichor/unknown")
    before = text[: text.index("movement_speed") + 3]
    (project / "rename.part").write_text(json.dumps(["res://gunner.pet", before.count("\n"), len(before.rsplit("\n", 1)[1]), "movement_pace"]), encoding="utf-8")
    (project / "rename.part").replace(project / "rename")
    editor.wait_for(b"lsp rename:")
    client.notify("textDocument/didClose", {"textDocument": {"uri": gunner}})
    tab = re.findall(r"lsp rename: (.*)", editor.output.decode("utf-8", "replace"))
    renamed = text.replace("movement_speed", "movement_pace").replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
    expect("lsp: a rename in Godot's tab of a script the client has with carriage returns renames the name at the caret in place", [line.rstrip("\r") for line in tab] == [renamed], f"{tab}\n{renamed}")


def check_refactoring_client_open(client, project, uri, editor):
    """A refactoring in Godot of draft.pet, which the client has open: refused
    while the client has unsaved changes to it, and written once it has none.
    The loaded script then takes the file's text, which the client reloads."""
    draft = uri("draft.pet")
    path = project / "draft.pet"
    text = path.read_text(encoding="utf-8")
    start = len(text[: text.index("oranges")].encode())

    def refactor():
        hold(project, editor, "refactor", b"lsp refactor:", json.dumps(["res://draft.pet", start, start + 7, "oranges", "pears"]))

    def probe():
        return hold(project, editor, "probe", b"lsp probe:", "res://draft.pet").split(" ")

    client.open(draft, "# From the client.\n" + text)
    client.request("petrichor/unknown")
    refactor()
    refused = path.read_text(encoding="utf-8")
    client.notify("textDocument/didChange", {"textDocument": {"uri": draft, "version": 2}, "contentChanges": [{"text": text}]})
    client.request("petrichor/unknown")
    probe()
    refactor()
    written = path.read_text(encoding="utf-8")
    after = probe()
    client.notify("textDocument/didClose", {"textDocument": {"uri": draft}})
    expect(
        "lsp: a refactoring in Godot of a script the client has open is refused while it has unsaved changes there, and written once it has none, the loaded script taking the file's text",
        refused == text and written == text.replace("oranges", "pears") and after[0] == "true",
        f"{refused!r}\n{written!r}\n{after}",
    )


def check_rename_in_godot_client_open(client, project, uri, editor):
    """A rename in Godot of weapon.pet's member, which gunner.pet and
    blaster.tres use, while the client has both scripts open as their files
    are. Godot writes the files, and the client reloads its documents a tenth
    of a second later, as VS Code does. Nothing is read against the client's
    texts from before, and Ctrl+Z in Godot then undoes the rename in every
    file."""
    names = ["weapon.pet", "gunner.pet", "blaster.tres"]
    before = {name: (project / name).read_bytes().decode() for name in names}
    for name in names[:2]:
        client.open(uri(name), before[name])
    client.request("petrichor/unknown")
    start = len(editor.output)
    member = re.search(r"pub var (\w+)", before["weapon.pet"])
    above = before["weapon.pet"][: member.start(1)]
    hold(project, editor, "rename", b"lsp rename:", json.dumps(["res://weapon.pet", above.count("\n"), len(above.rsplit("\n", 1)[1]), "spray", True]))
    renamed = {name: (project / name).read_bytes().decode() for name in names}

    def reload(version, texts):
        time.sleep(0.1)
        for name in names[:2]:
            client.notify("textDocument/didChange", {"textDocument": {"uri": uri(name), "version": version}, "contentChanges": [lines_changed(before[name] if version == 2 else renamed[name], texts[name])]})
        client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": uri(name), "type": 2} for name in names]})
        client.request("petrichor/unknown")

    reload(2, renamed)
    hold(project, editor, "keys", b"lsp keys:", json.dumps(["res://weapon.pet", "ui_undo"]))
    undone = {name: (project / name).read_bytes().decode() for name in names}
    reload(3, undone)
    for name in names[:2]:
        client.notify("textDocument/didClose", {"textDocument": {"uri": uri(name)}})
    printed = [line for line in editor.output[start:].decode("utf-8", "replace").splitlines() if "ERROR" in line or "WARNING" in line]
    expect(
        "lsp: a rename in Godot of scripts the client has open as their files reads nothing against the client's old texts, and Ctrl+Z undoes it",
        all("spray" in renamed[name] for name in names) and undone == before and not printed,
        f"{renamed}\n{undone}\n{printed}",
    )


def check_conflicts(client, project, editor, port):
    """Files changed on disk while Godot holds unsaved changes of them
    (architecture §6.5). arena.tscn, open without unsaved changes, is reloaded
    once it changes on disk. Then note.pet, given an unsaved change in a tab,
    changed and then deleted, arena.tscn, given one, and ramp.tres, edited
    through the undo history, conflict. Each is told once to every session and
    listed on request, and a rename that must save arena.tscn is refused. A
    conflict ends once its file is back as Godot saw it; ramp.tres stays edited."""
    disk = LspClient(port)
    disk.initialize(view="disk")
    arena, note = project / "arena.tscn", project / "note.pet"
    note.write_text("extends Node\n", encoding="utf-8")
    saved = {path: path.read_bytes() for path in (arena, note)}

    def unsaved():
        """The files the request lists, by name, with whether each conflicts."""
        listed = client.request("petrichor/unsavedFiles")
        return {file_name(each["uri"]): each["conflict"] for each in listed["result"]} if listed and "result" in listed else listed

    def told(each):
        message = each.take(lambda m: m.get("method") == "window/showMessage", 0)
        return message and message["params"]["message"]

    arena.write_bytes(saved[arena] + b'\n[node name="Extra" type="Node" parent="."]\n')
    unsaved()
    reloaded = hold(project, editor, "scene", b"lsp scene:")
    arena.write_bytes(saved[arena])
    unsaved()
    expect("lsp: an open scene without unsaved changes is reloaded once it changes on disk", reloaded == "true" and hold(project, editor, "scene", b"lsp scene:") == "false", reloaded)

    hold(project, editor, "dirty", b"lsp dirty:", "res://note.pet")
    note.write_bytes(saved[note] + b"# Written on disk.\n")
    first = unsaved()
    tab = [told(client), disk.take(lambda m: m.get("method") == "window/showMessage", 5)]
    note.write_bytes(saved[note] + b"# Written on disk again.\n")
    again = unsaved()
    expect(
        "lsp: a file changed on disk while Godot's tab of it is unsaved is told once to every session, and listed",
        first == {"note.pet": True} and again == first and tab[0] and "res://note.pet" in tab[0] and tab[1] and tab[1]["params"]["message"] == tab[0] and told(client) is None,
        f"{first}\n{again}\n{tab}",
    )
    # The client has the file's text, which differs from the tab because of
    # the conflict, so the conflict is what a refactoring tells.
    client.open(file_uri(note), note.read_bytes().decode())
    client.request("petrichor/unknown")
    hud = (project / "hud.pet").read_bytes()
    at = hud.index(b"show_health")
    refused = hold(project, editor, "call", b"lsp call:", json.dumps(["_change_scripts", [["res://note.pet", 0, 7, "extends", "extends"], ["res://hud.pet", at, at + 11, "show_health", "show_life"]], True]))
    client.notify("textDocument/didClose", {"textDocument": {"uri": file_uri(note)}})
    expect(
        "lsp: a refactoring of a script whose unsaved tab conflicts with its file is refused as a conflict, though the client's text differs from the tab",
        "Godot has unsaved changes to res://note.pet, which changed on disk, so nothing was changed." in refused and "note.pet: its text in the LSP client" not in refused and (project / "hud.pet").read_bytes() == hud,
        refused,
    )

    note.write_bytes(saved[note])
    back = unsaved()
    note.unlink()
    gone = unsaved()
    deleted = told(client)
    expect(
        "lsp: a file deleted on disk while Godot's tab of it is unsaved is a conflict, told with how to write it back",
        back == {"note.pet": False} and gone == {"note.pet": True} and deleted and "res://note.pet" in deleted and "Ctrl+S" in deleted,
        f"{back}\n{gone}\n{deleted}",
    )

    hold(project, editor, "dirty", b"lsp dirty:")
    arena.write_bytes(saved[arena] + b"\n")
    scene = unsaved()
    renamed = hold(project, editor, "call", b"lsp call:", json.dumps(["_rename_declaration", 0, "res://hud.pet", "show_health", "show_life", [], False, True]))
    expect(
        "lsp: a rename that must save an open scene which changed on disk while unsaved is refused",
        scene == {"note.pet": True, "arena.tscn": True} and "arena.tscn" in (told(client) or "") and renamed
        and arena.read_bytes() == saved[arena] + b"\n" and "show_health" in (project / "hud.pet").read_text(encoding="utf-8"),
        f"{scene}\n{renamed}",
    )

    ramp = project / "ramp.tres"
    saved[ramp] = ramp.read_bytes()
    hold(project, editor, "dirty", b"lsp dirty:", "res://ramp.tres")
    ramp.write_bytes(saved[ramp].replace(b"width = 64", b"width = 32"))
    resource = unsaved()
    replaced = told(client)
    expect(
        "lsp: a resource edited in Godot that changed on disk is a conflict, told that the file replaced the edit",
        resource == {"note.pet": True, "arena.tscn": True, "ramp.tres": True} and replaced and "res://ramp.tres" in replaced and "replaced" in replaced,
        f"{resource}\n{replaced}",
    )

    for path in (arena, note, ramp):
        path.write_bytes(saved[path])
    ended = unsaved()
    disk.socket.close()
    expect(
        "lsp: a conflict ends once its file is again as Godot saw it, but an edited resource's lasts until Godot saves it",
        ended == {"note.pet": False, "arena.tscn": False, "ramp.tres": True},
        str(ended),
    )


def check_sessions(project, uri, port):
    """Sessions served at once (architecture §2.4). Those on the human's view
    share the workspace, its text of a file being the session's that changed
    it last, and each sees its own documents. One on the disk view sees the
    files with its own documents."""
    supply, demand, rival, probe = (uri(name) for name in ["supply.pet", "demand.pet", "rival.pet", "probe.pet"])
    supply_text = (project / "supply.pet").read_text(encoding="utf-8")
    gone = supply_text.replace("amount", "count")
    demand_text = 'extends Node\n\nconst Supply = preload("supply.pet")\n\n\nfunc need(supply: Supply) -> int:\n\treturn supply.amount()\n'
    unknown = LspClient(port)
    refused = unknown.initialize(view="human")
    unknown.socket.close()
    expect("lsp: an unknown view fails initialize, saying so", refused and '"human"' in refused.get("error", {}).get("message", ""), str(refused))

    a, b, disk = LspClient(port), LspClient(port), LspClient(port)
    replies = [a.initialize(), b.initialize(), disk.initialize(view="disk")]
    a.open(supply, gone)
    b.open(demand, demand_text)
    disk.open(demand, demand_text)
    own = a.diagnostics(supply, lambda p: True)
    shared = b.diagnostics(demand, lambda p: says(p, "amount"))
    apart = disk.diagnostics(demand, lambda p: says(p, "amount"), 3)
    (project / "supply.pet").write_text(gone, encoding="utf-8")
    a.notify("textDocument/didSave", {"textDocument": {"uri": supply}})
    saved = disk.diagnostics(demand, lambda p: says(p, "amount"))
    expect(
        "lsp: sessions are served at once; the human's view has another session's unsaved text, the disk view only once it is saved",
        all(reply and "result" in reply for reply in replies) and own and says(shared, "amount") and apart and not says(apart, "amount") and says(saved, "amount"),
        f"{replies}\n{own}\n{shared}\n{apart}\n{saved}",
    )
    # The disk view answers from a file added just before, with no notification (architecture §6.5).
    fresh = uri("fresh_use.pet")
    fresh_text = "extends Node\n\n\nfunc f() -> int:\n\treturn Fresh.N\n"
    disk.open(fresh, fresh_text)
    (project / "fresh.pet").write_text("class_name Fresh extends RefCounted\n\npub const N := 4\n", encoding="utf-8")
    hover = disk.request("textDocument/hover", {"textDocument": {"uri": fresh}, "position": lsp_position(fresh_text, fresh_text.index("N\n"))})
    disk.notify("textDocument/didClose", {"textDocument": {"uri": fresh}})
    expect("lsp: the disk view's next answer reads a file added on disk, unannounced", hover and "Fresh.N: int" in str(hover.get("result")), str(hover))
    (project / "supply.pet").write_text(supply_text, encoding="utf-8")
    a.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": supply, "type": 2}]})
    restored = disk.diagnostics(demand, lambda p: not says(p, "amount"))
    a.notify("textDocument/didClose", {"textDocument": {"uri": supply}})
    closed = b.diagnostics(demand, lambda p: not says(p, "amount"))
    a.open(supply, gone)
    reopened = b.diagnostics(demand, lambda p: says(p, "amount"))
    a.socket.close()
    left = b.diagnostics(demand, lambda p: not says(p, "amount"))
    expect(
        "lsp: a document closed, or a session gone, no longer counts for the others",
        restored and not says(restored, "amount") and closed and not says(closed, "amount") and says(reopened, "amount") and left and not says(left, "amount"),
        f"{restored}\n{closed}\n{reopened}\n{left}",
    )

    # One file with an unsaved text in each of two sessions: each is answered for its own.
    c = LspClient(port)
    c.initialize()
    texts = {b: 'extends Node\n\n\nfunc f() -> void:\n\tvar shade := 1\n\tprint(shade, missing_b)\n', c: 'extends Node\n\n\nfunc f() -> void:\n\tvar shade := "x"\n\n\tprint(shade, missing_c)\n'}
    for client, text in texts.items():
        client.open(rival, text)
    lines = []
    hovers = []
    for client, text, name in [(b, texts[b], "missing_b"), (c, texts[c], "missing_c")]:
        published = client.diagnostics(rival, lambda p, name=name: says(p, name))
        lines.append([d["range"]["start"]["line"] for d in published["diagnostics"] if "missing" in d["message"]] if published else None)
        hover = client.request("textDocument/hover", {"textDocument": {"uri": rival}, "position": lsp_position(text, text.index("shade"))})
        hovers.append(hover and hover["result"]["contents"]["value"])
    expect(
        "lsp: two sessions with unsaved texts of one file are each answered for their own",
        lines == [[5], [6]] and hovers == ["```petrichor\nvar shade: int\n```", "```petrichor\nvar shade: String\n```"],
        f"{lines}\n{hovers}",
    )
    stale = {"title": "x", "data": {"uri": rival, "revision": 1, "files": ["res://rival.pet"], "title": "x", "begin": 0, "end": 0, "message": "x", "file": ""}}
    c.request("codeAction/resolve", stale)
    told = [client.take(lambda m: m.get("method") == "window/showMessage", 0.5) for client in (c, b)]
    expect("lsp: a message goes to the session whose request caused it", told[0] and told[1] is None, str(told))

    # A session's text of a script counts over Godot's unsaved tab, until it closes it.
    user = uri("user.pet")
    probe_text = 'extends Node\n\nconst User = preload("user.pet")\n\n\nfunc g(user: User) -> int:\n\treturn user.extra()\n'
    b.open(probe, probe_text)
    tab = b.diagnostics(probe, lambda p: says(p, "extra"))
    c.open(user, (project / "user.pet").read_text(encoding="utf-8") + "\n\npub func extra() -> int:\n\treturn 1\n")
    taken = b.diagnostics(probe, lambda p: not says(p, "extra"))
    c.notify("textDocument/didClose", {"textDocument": {"uri": user}})
    back = b.diagnostics(probe, lambda p: says(p, "extra"))
    expect("lsp: a session's text counts over Godot's tab, which counts again once it is closed", says(tab, "extra") and taken and not says(taken, "extra") and says(back, "extra"), f"{tab}\n{taken}\n{back}")
    for client in (b, c, disk):
        client.socket.close()


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
        commands=["locals", "members", "s", "members", "c", "c", "locals", "c", "c"],
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
            "Debugger Break, Reason: 'Breakpoint'",
            "*Frame 0 - res://language/breakpoint.pet:15 in function 'pause'",
            "breakpoint: pause returned 5",
        ],
        ["Division by zero."],
    )

    # The editor's evaluator reads the frame's instance and keeps the break.
    code, output, answered = run_evaluate(godot)
    expect("debugger evaluate", answered and code == 0 and "breakpoint: pause returned 5" in output, output)

    code, output = run_debugger(
        godot, "-d", "--path", str(PROJECT), "--script", "res://language/coroutines_debug.gd",
        commands=["bt", "c", "bt", "c"],
    )
    expect_run(
        "coroutines debugger", code, output,
        ["*Frame 0 - res://language/coroutine_check.pet:7 in function 'twice'",
         "*Frame 0 - res://language/coroutine_check.pet:22 in function 'shared'",
         "coroutines debugger: done"],
        ["Await result: expected int, found String"] + expected_errors(output, "coroutines debugger"),
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
            "hot reload: 0 failure(s)",
        ],
    )


def run_export_groups(godot):
    # The libraries that exported games load, without the compiler: an export
    # looks for the release one, and the exported pack runs with each.
    stale = [target for target in ("template_debug", "template_release") if not library_current(target)]
    for target in stale:
        expect(f"export: the {target} library is built from the current sources", False, f"{PROJECT / host_library(target)} is missing." if PREBUILT else f"Build it with: scons target={target}")
    if stale:
        return
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pack = tmp / "game.pck"
        # A script with errors is left out, not exported as source (spec §1.3).
        broken = PROJECT / "broken_export_check.pet"
        broken.write_text('extends Node\n\nfunc f() -> int:\n\treturn "not an int"\n', encoding="utf-8")
        try:
            code, output = run(godot, "--path", str(PROJECT), "--export-pack", HOST_PLATFORM, str(pack))
        finally:
            broken.unlink(missing_ok=True)
            broken.with_name(broken.name + ".uid").unlink(missing_ok=True)
        expected = ("broken_export_check.pet", "Cannot return a value of type String as int.")
        unexpected = [line for line in output.splitlines() if "ERROR" in line and not any(part in line for part in expected)]
        expect("export", code == 0 and not unexpected, output)
        expect("export: a script with errors is left out", "broken_export_check.pet has errors and is left out of the export." in output, output)
        # The editor's window says it failed; the command line, as a warning.
        expect("export: a script with errors is an export error", "completed with warnings" in output, output)
        expect("export: the extension has a library for the target, for the engine as for this language", lacks_library(output) == (False, False), output)

        # A value that the main scene lacks fails the headless build and the
        # export, but the script compiles, and is exported (spec §7.5). The
        # editor takes no override.cfg, so a project of its own names the scene;
        # with extension_list.cfg, the editor finds the extension at start.
        unassigned = tmp / "unassigned"
        (unassigned / ".godot").mkdir(parents=True)
        (unassigned / "bin").mkdir()
        (unassigned / "project.godot").write_text('config_version=5\n\n[application]\n\nrun/main_scene="res://unassigned_export_check.tscn"\nconfig/features=PackedStringArray("4.7")\n', encoding="utf-8")
        for file in ("export_presets.cfg", "petrichor.gdextension", ".godot/extension_list.cfg", host_library("editor")):
            shutil.copy(PROJECT / file, unassigned / file)
        (unassigned / "unassigned_export_check.pet").write_text("extends Node\n\n@export pub var target: Node\n", encoding="utf-8")
        # The scene's own check (architecture §6.4) reports it, and an abstract
        # class on a node (spec §4.1), whose script compiles too.
        (unassigned / "abstract_export_check.pet").write_text("@abstract\nextends Node\n", encoding="utf-8")
        (unassigned / "unassigned_export_check.tscn").write_text('[gd_scene format=3]\n\n[ext_resource type="Script" path="res://unassigned_export_check.pet" id="1"]\n[ext_resource type="Script" path="res://abstract_export_check.pet" id="2"]\n\n[node name="Root" type="Node"]\n\n[node name="Child" type="Node" parent="."]\nscript = ExtResource("1")\n\n[node name="Unit" type="Node" parent="."]\nscript = ExtResource("2")\n', encoding="utf-8")
        # So does a resource file's check: a struct it stores that does not
        # decode (spec §4.3).
        (unassigned / "loot_export_check.pet").write_text("extends Resource\n\npub struct Hit:\n\tvar damage: int\n\n@export pub var hit: Hit?\n", encoding="utf-8")
        (unassigned / "loot_export_check.tres").write_text('[gd_resource type="Resource" format=3]\n\n[ext_resource type="Script" path="res://loot_export_check.pet" id="1"]\n\n[resource]\nscript = ExtResource("1")\nhit = {\n"damage": "5"\n}\n', encoding="utf-8")
        code, built = run(godot, "-e", "--path", str(unassigned), "--", "--petrichor-build")
        code_export, output = run(godot, "--path", str(unassigned), "--export-pack", HOST_PLATFORM, str(tmp / "unassigned.pck"))
        lacking = 'res://unassigned_export_check.tscn: Child has no value for "target", an export that must be assigned.'
        abstract = "res://unassigned_export_check.tscn: Unit has abstract_export_check.pet, which is abstract and cannot be set on a node."
        loot = 'res://loot_export_check.tres: the value stored for "hit" of the resource does not fit its type: '
        expect("export: the headless build fails on a value that a scene lacks", code != 0 and "3 script(s), 1 scene(s), 1 resource file(s), 2 with errors" in built and lacking in built, built)
        expect("export: a value that a scene lacks is an export error", code_export == 0 and lacking in output and "has errors and is left out" not in output, output)
        expect("export: the headless build and the export fail on the checks of a scene and a resource file", all(error in built and error in output for error in (abstract, loot)), built + output)

        # An export whose game could not load the extension fails
        # (architecture §5). The copy lacks the file of the release library.
        # For the Web the extension lists no library, and the preset has no
        # extension support.
        expect("export: a library that the project lacks is an export error", f"res://petrichor.gdextension names res://{host_library('template_release')} as the library for a release export to " in output and lacks_library(output) == (False, False), output)
        presets = unassigned / "export_presets.cfg"
        web = len(re.findall(r"^\[preset\.\d+\]$", presets.read_text(encoding="utf-8"), flags=re.M))
        with open(presets, "a", encoding="utf-8") as file:
            file.write(f'\n[preset.{web}]\n\nname="Web"\nplatform="Web"\nexport_filter="all_resources"\ninclude_filter=""\nexclude_filter=""\n\n[preset.{web}.options]\n\nvariant/extensions_support=false\n')
        code, output = run(godot, "--path", str(unassigned), "--export-pack", "Web", str(tmp / "web.pck"))
        expect("export: a target without a library is an export error", "res://petrichor.gdextension has no library for a release export to Web wasm32. The exported game cannot load Petrichor, so its .pet scripts will not run." in output and lacks_library(output) == (True, True), output)
        expect("export: a Web export without extension support is an export error", 'This Web export has "Extensions Support" turned off.' in output, output)

        # A project directory without sources, whose only library is one that
        # exported games load.
        # Spec §8.3: a failed assert ends the function in a debug build, and is
        # removed in a release build.
        for target, after_assert, errors in (("template_debug", 0, ["Assertion failed."]), ("template_release", 1, [])):
            library = host_library(target)
            runner = tmp / f"runner_{target}"
            (runner / ".godot").mkdir(parents=True)
            (runner / library).parent.mkdir()
            shutil.copy(PROJECT / "project.godot", runner)
            shutil.copy(PROJECT / ".godot" / "extension_list.cfg", runner / ".godot")
            shutil.copy(PROJECT / library, runner / library)
            # Under the key "debug": an editor binary has no "release" feature
            # (core/os/os.cpp:481-511), and loads the library of an entry whose
            # tags it all has (core/extension/gdextension_library_loader.cpp:82-96).
            configuration = (PROJECT / "petrichor.gdextension").read_text(encoding="utf-8").split("[libraries]")[0]
            (runner / "petrichor.gdextension").write_text(f'{configuration}[libraries]\n\ndebug = "{library}"\n', encoding="utf-8")
            code, output = run(godot, "--path", str(runner), "--script", str(HERE / "export_check.gd"), "--", str(pack))
            expect_run(f"exported pack run with the {target} library", code, output, ["export check: greeter.petc header = PETC", f"export check: after a failed assert: {after_assert}", *GAME_OUTPUT], errors)


def libraries():
    """The entries of [libraries] in the test project's petrichor.gdextension,
    in order: each one's tags and its library's path in the project."""
    config = configparser.ConfigParser(interpolation=None)
    config.read(PROJECT / "petrichor.gdextension", encoding="utf-8")
    return [(key.split("."), path.strip('"')) for key, path in config["libraries"].items()]


def host_library(target):
    """The path of the library of SCons's `target` that this host loads: that
    of the entry with the most tags, all of them the host's or the target's,
    and the first of equals (core/extension/gdextension_library_loader.cpp:82-97)."""
    features = HOST_FEATURES | {target.removeprefix("template_")}
    best, most = None, 0
    for tags, path in libraries():
        if set(tags) <= features and len(tags) > most:
            best, most = path, len(tags)
    return best


def lacks_library(output):
    """Whether an export found no library of the extension for its target: by
    the engine's own plugin, which warns
    (editor/export/gdextension_export_plugin.h:161-165), and by this
    language's, which repeats the engine's rule; they must agree."""
    engine = any("WARNING" in line and "res://petrichor.gdextension" in line for line in output.splitlines())
    return engine, "res://petrichor.gdextension has no library for " in output


def project_build_stamp(godot):
    """Fingerprint of every input and compiled script of PROJECT, so a
    single-group run rebuilds when any project file, scene or binary changes,
    including deletions."""
    bin_dir = PROJECT / "bin"
    editor_libs = sorted(p for p in bin_dir.glob("*editor*") if p.is_file()) if bin_dir.is_dir() else []
    godot_bin = Path(shutil.which(godot) or godot)
    if not editor_libs or not (PROJECT / ".godot" / "imported").is_dir():
        return None
    try:
        godot_stat = godot_bin.stat()
        lib_stats = [(lib.name, lib.stat()) for lib in editor_libs]
    except OSError:
        return None

    digest = hashlib.sha256()
    digest.update(f"godot:{godot_bin.resolve()}:{godot_stat.st_mtime_ns}:{godot_stat.st_size}\0".encode())
    for name, stat in lib_stats:
        digest.update(f"lib:{name}:{stat.st_mtime_ns}:{stat.st_size}\0".encode())

    scripts = []
    for dirpath, dirnames, filenames in os.walk(PROJECT):
        dirnames[:] = sorted(d for d in dirnames if d not in (".godot", "bin", "__pycache__"))
        for name in sorted(filenames):
            full = Path(dirpath, name)
            rel = full.relative_to(PROJECT).as_posix()
            data = full.read_bytes()
            digest.update(f"proj:{rel}:{len(data)}\0".encode())
            digest.update(data)
            if rel.endswith(".pet"):
                scripts.append(rel)

    if not scripts:
        return None
    for rel in scripts:
        petc = PROJECT / ".godot" / "petrichor" / (rel + "c")
        try:
            data = petc.read_bytes()
        except OSError:
            return None
        digest.update(f"petc:{rel}:{len(data)}\0".encode())
        digest.update(data)

    return digest.hexdigest()


def project_bytecode_current(godot):
    """Whether PROJECT and its compiled bytecode match the last clean build."""
    try:
        recorded = BUILD_STAMP.read_text(encoding="utf-8").strip()
    except OSError:
        return False
    return bool(recorded) and project_build_stamp(godot) == recorded


def library_current(target):
    """Whether the library of `target` is built from the current sources: a
    dry run of SCons that explains itself names each file it would build. Its
    output answers, not its exit code, which a launcher script may lose. A
    library built elsewhere only has to exist."""
    if PREBUILT:
        return (PROJECT / host_library(target)).is_file()
    scons = shutil.which("scons")
    if scons is None:
        return False
    output = subprocess.run([scons, f"target={target}", "-n", "--debug=explain"], cwd=ROOT, capture_output=True).stdout
    return b"scons: done building targets." in output and not re.search(rb"building `|is being rebuilt", output)


if __name__ == "__main__":
    main()
