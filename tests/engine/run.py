"""Engine tests: run the test project headless with a Godot 4.7 editor binary.

Usage:
    scons target=editor
    scons target=template_debug      (for the export group)
    scons target=template_release    (for the export group)
    python tests/engine/run.py <godot editor binary> [group...]    (or set GODOT)

Checks the game run, the editor side (placeholders, saving), the language, the
debugger, hot reload, an exported pack run in a directory without sources with
each library of export templates, and the FPS starter kit and Ring's breakout
with their migrated scripts. Naming groups runs only those, after the build;
the groups are those of GROUPS, hot reload being part of debugger.

Environment:
    PETRICHOR_PREBUILT=1          the libraries in tests/engine/project/bin were
                                  built elsewhere: they only have to exist
    PETRICHOR_TEST_TIMEOUT=<s>    the limit of each engine run, 60 s by default
"""

import configparser
import hashlib
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

GROUPS = ["game", "editor", "language", "debugger", "export", "fps", "ring"]

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
                    shutil.copy(HOT_RELOAD_BYTECODE / f"v{version}.petc", live[1])
                    send_editor_message(connection, "reload_scripts", [LIVE_SCRIPT])
                    send_editor_message(connection, "hot_reload_test:reloaded", [])
                return running.finish()
        finally:
            restore_files(originals)


def run_stale_bytecode(godot):
    """Spec §8.1: a call to a method of this language checks that the callee
    still has the signature the caller was compiled against, and a class loads
    only with the base it was compiled against. Within a run that always holds
    (§12.4); a run from bytecode that was not built together, here callee.pet
    and inherit_base.pet replaced by other versions while the scripts that use
    them are not built again, is the way to break it."""
    code, output = run(godot, "--path", str(PROJECT), "res://language/stale/stale_test.tscn")
    expect_run("stale bytecode: as built", code, output, ["stale: 2", "stale inheritance: valid: 1"])
    replaced = ("callee", "inherit_base")
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
    refused = "inherit_derived.pet Base method declaration changed. Rebuild the project."
    expect("stale bytecode: a class whose base changed is refused", refused in output and "stale inheritance: rejected" in output, output)


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
    # the original, so only the path stays.
    for file in [*project.rglob("*.tscn"), *project.rglob("*.tres"), project / "project.godot"]:
        lines = file.read_text(encoding="utf-8").split("\n")
        for i, line in enumerate(lines):
            for old, new in replaced.items():
                if f'"{old}"' in line or f'"*{old}"' in line:
                    lines[i] = re.sub(r' uid="[^"]*"', "", line).replace(old, new)
        file.write_text("\n".join(lines), encoding="utf-8")


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

    # The fps and ring groups prepare projects of their own, so they run beside
    # the groups of the test project.
    finishes = []
    if wanted("fps"):
        finishes.append(beside("fps", run_migration_group, godot, "fps", prepare_fps, FPS_MIGRATED))
    if wanted("ring"):
        finishes.append(beside("ring", run_migration_group, godot, "ring", prepare_ring, RING_MIGRATED, "res://ring_check.tscn"))

    if not only or (only - {"fps", "ring"} and ("editor" in only or not project_bytecode_current(godot))):
        # The game reads the bytecode the editor writes (architecture §2.4);
        # the build waits for the first scan, which imports the project.
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


def run_migration_group(godot, name, prepare, migrated_scripts, *scene):
    """A real project runs with its migrated scripts (architecture §5 steps 4
    and 6). The original reports errors of its own in a headless run, such as
    the FPS kit's sounds still playing at exit and weapon models it removes
    without freeing, so the migrated run may only repeat those, and keep no
    resource at exit, other than sounds, that the original does not."""
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
        # scripts written by hand are not.
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-format")
        changed = re.search(r", (\d+) formatted,", output)
        expect(f"{name} format", code == 0 and ", 0 with errors" in output and changed and int(changed.group(1)) > 0, output)
        code, output = run(godot, "-e", "--path", str(migrated), "--", "--petrichor-format", "--check")
        expect(f"{name} format check: then they are", code == 0 and " 0 not formatted," in output, output)
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
            f"{LIVE_SCRIPT}: not reloaded, since an earlier change needs the game to run again.",
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
        loot = 'res://loot_export_check.tres: the value stored for "hit" of the resource does not decode: '
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

    for source_dir in ("src/bytecode", "src/compiler", "src/adapter/editor", "gen"):
        base = ROOT / source_dir
        if base.is_dir():
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames.sort()
                for name in sorted(filenames):
                    full = Path(dirpath, name)
                    stat = full.stat()
                    rel = full.relative_to(ROOT).as_posix()
                    digest.update(f"src:{rel}:{stat.st_mtime_ns}:{stat.st_size}\0".encode())

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
