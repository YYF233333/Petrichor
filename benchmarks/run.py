"""Paired runtime and project-startup benchmarks (architecture §4).

Usage:
    scons target=editor
    python benchmarks/run.py <Godot 4.7 official editor binary> [--group GROUP ...]
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE.parent / "tests" / "engine" / "project"
STARTUP_SCRIPTS = 32
STARTUP_SAMPLES = 3
GROUPS = ("compute", "boundary", "allocation", "startup")
# The logical CPU the runtime cases run on. On a CPU with two kinds of cores,
# such as Zen 5 with Zen 5c, a case's time otherwise falls into two modes.
RUNTIME_CPU = 2


def run(godot, *args, cpu=None):
    command = [str(godot), "--headless", *map(str, args)]
    launched, preexec = command, None
    if cpu is not None and os.name == "nt":
        # `start` sets the affinity, which the engine process that the console
        # executable starts inherits; cmd exits with the engine's exit code.
        line = subprocess.list2cmdline(["start", "/wait", "/b", "/affinity", format(1 << cpu, "X"), "", *command])
        launched = f'cmd /v:on /c "{line} & exit !errorlevel!"'
    elif cpu is not None and hasattr(os, "sched_setaffinity"):
        preexec = lambda: os.sched_setaffinity(0, {cpu})
    start = time.perf_counter_ns()
    result = subprocess.run(launched, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600, preexec_fn=preexec)
    elapsed = (time.perf_counter_ns() - start) / 1_000_000
    output = result.stdout + result.stderr
    if result.returncode != 0 or re.search(r"\bERROR\b|leaked at exit", output):
        raise RuntimeError(f"{' '.join(command)}\n{output}")
    return output, elapsed


def import_project(godot, project):
    # Not checked, as import_project() of tests/engine/run.py explains.
    subprocess.run([str(godot), "--headless", "--path", str(project), "--import"], capture_output=True, timeout=600)


def build(godot, project, count):
    output, elapsed = run(godot, "-e", "--path", project, "--", "--petrichor-build")
    if f"petrichor build: {count} script(s), 0 with errors" not in output:
        raise RuntimeError(output)
    return elapsed


def copy_extension(project):
    shutil.copytree(TESTS / "bin", project / "bin")
    shutil.copy(TESTS / "petrichor.gdextension", project)


def runtime(godot, root, groups):
    project = root / "runtime"
    shutil.copytree(HERE / "project", project, ignore=shutil.ignore_patterns(".godot", "bin"))
    copy_extension(project)
    import_project(godot, project)
    cases = sorted((project / "cases").glob("*.pet"))
    build(godot, project, len(cases))
    output, _ = run(godot, "--path", project, "--", *groups, cpu=RUNTIME_CPU)
    rows = [json.loads(line.removeprefix("bench: ")) for line in output.splitlines() if line.startswith("bench: ")]
    # The manifest and files must agree even when only one group is selected.
    manifest = dict(re.findall(r'"(\w+)": \[[\d_]+, "(\w+)"\]', (project / "bench.gd").read_text(encoding="utf-8")))
    names = sorted(path.stem for path in cases)
    if sorted(manifest) != names or names != sorted(path.stem for path in (project / "cases").glob("*.gd")):
        raise RuntimeError("The benchmark manifest and paired case files differ")
    expected = sorted(name for name, group in manifest.items() if group in groups)
    if sorted(row["case"] for row in rows) != expected:
        raise RuntimeError(f"Cases measured do not match {expected}\n{output}")
    for row in rows:
        if not row["same"] or any(len(row[key]) != 5 or any(value <= 0 for value in row[key]) for key in ("gdscript_usec", "petrichor_usec")):
            raise RuntimeError(f"Invalid samples or unequal results: {row}")
        gd, pet = (statistics.median(row[key]) / 1000 for key in ("gdscript_usec", "petrichor_usec"))
        print(f"{row['case']:<22}{gd:>10.2f}{pet:>12.2f}{gd / pet:>10.2f}x", flush=True)


def script_source(extension, unit, change=0):
    visibility = "pub " if extension == "pet" else ""
    return ("extends RefCounted\n\n" + f"{visibility}func value() -> int:\n"
            + "\tvar total := 0\n\tfor i in 64:\n"
            + f"\t\ttotal += (i + {unit}) % 7\n\treturn total + {change}\n")


def startup_project(root, extension):
    project = root / extension
    (project / "scripts").mkdir(parents=True)
    for unit in range(STARTUP_SCRIPTS):
        (project / "scripts" / f"unit_{unit}.{extension}").write_text(script_source(extension, unit), encoding="utf-8")
    shutil.copy(HERE / "startup" / "main.gd", project)
    (project / "main.tscn").write_text(
        '[gd_scene format=3]\n\n[ext_resource type="Script" path="res://main.gd" id="1"]\n\n'
        '[node name="Startup" type="Node"]\nscript = ExtResource("1")\n', encoding="utf-8")
    config = ('config_version=5\n\n[application]\nconfig/name="Startup benchmark"\n'
              'run/main_scene="res://main.tscn"\nconfig/features=PackedStringArray("4.7")\n\n'
              f'[benchmark]\nextension="{extension}"\nscripts={STARTUP_SCRIPTS}\n')
    if extension == "pet":
        copy_extension(project)
    else:
        addon = project / "addons" / "prepare"
        addon.mkdir(parents=True)
        shutil.copy(HERE / "startup" / "prepare.gd", addon)
        (addon / "plugin.cfg").write_text(
            '[plugin]\nname="Prepare benchmark"\ndescription="Wait for the initial scan"\n'
            'author="Petrichor"\nversion="1.0"\nscript="prepare.gd"\n', encoding="utf-8")
        config += '\n[editor_plugins]\nenabled=PackedStringArray("res://addons/prepare/plugin.cfg")\n'
    (project / "project.godot").write_text(config, encoding="utf-8")
    return project


def startup(godot, root):
    rows = []
    checksum = sum((i + unit) % 7 for unit in range(STARTUP_SCRIPTS) for i in range(64))
    # Warm the executable/OS caches once; every measured cold project is freshly
    # imported and has never been built or run.
    for sample in range(-1, STARTUP_SAMPLES):
        pair = root / f"startup_{sample}"
        projects = {extension: startup_project(pair, extension) for extension in ("gd", "pet")}
        for project in projects.values():
            import_project(godot, project)
        order = ("gd", "pet") if sample % 2 == 0 else ("pet", "gd")
        for state in ("cold", "repeat", "edit"):
            row = {"state": state}
            for extension in order:
                project = projects[extension]
                if state == "edit":
                    source = project / "scripts" / f"unit_0.{extension}"
                    old = source.stat().st_mtime
                    source.write_text(script_source(extension, 0, 1), encoding="utf-8")
                    # Give the edit a distinct integer-second timestamp without a sleep.
                    os.utime(source, (old + 2, old + 2))
                if extension == "pet":
                    prepare = build(godot, project, STARTUP_SCRIPTS)
                else:
                    output, prepare = run(godot, "-e", "--path", project, "--", "--bench-prepare")
                    if "startup: prepared" not in output:
                        raise RuntimeError(output)
                output, launch = run(godot, "--path", project)
                expected = checksum + (state == "edit")
                if f"startup: {STARTUP_SCRIPTS} {expected}" not in output.splitlines():
                    raise RuntimeError(f"Expected checksum {expected}\n{output}")
                row[extension] = prepare + launch
            if sample >= 0:
                rows.append(row)
    print(f"Startup prepare + run medians, ms (one warm-up, {STARTUP_SAMPLES} samples):", flush=True)
    print(f"{'state':<22}{'GDScript':>10}{'Petrichor':>12}", flush=True)
    for state in ("cold", "repeat", "edit"):
        gd, pet = (statistics.median(row[ext] for row in rows if row["state"] == state) for ext in ("gd", "pet"))
        print(f"{state:<22}{gd:>10.1f}{pet:>12.1f}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("godot", type=Path)
    parser.add_argument("--group", choices=GROUPS, action="append", help="repeat to select groups; default: all")
    args = parser.parse_args()
    godot = args.godot.resolve()
    version, _ = run(godot, "--version")
    version = version.strip().splitlines()[-1]
    if ".official." not in version:
        parser.error("Use the official optimized engine for the GDScript comparison")
    groups = args.group or list(GROUPS)
    print(f"engine: {version}", flush=True)
    with tempfile.TemporaryDirectory(prefix="petrichor-bench-") as tmp:
        root = Path(tmp)
        selected = [group for group in groups if group != "startup"]
        if selected:
            print("Runtime medians, ms (one warm-up, five samples):", flush=True)
            print(f"{'case':<22}{'GDScript':>10}{'Petrichor':>12}{'GD/Pet':>11}", flush=True)
            runtime(godot, root, selected)
        if "startup" in groups:
            startup(godot, root)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
