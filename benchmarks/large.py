"""The build of a generated project of 1000 scripts, held to a budget of
counts that are the same on every machine (architecture §4). The times are
printed and not held.

Usage:
    scons target=editor
    python benchmarks/large.py <Godot 4.7 editor binary>
"""

import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from run import copy_extension, import_project, run

SCRIPTS, SCENES, GD, DEPTH = 1000, 300, 60, 30
BUDGET = {"compiles outside the analysis": 0, "scene loads": 590, "checks": 1300}


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def generate(project):
    """Scripts that each take the one before, scenes with two of them on their
    nodes that each instance the one before, DEPTH deep, and GDScript files."""
    write(project / "project.godot", 'config_version=5\n\n[application]\n\nconfig/name="Large"\n')
    copy_extension(project)
    for i in range(SCRIPTS):
        prev = f", prev: S{i - 1:04}" if i else ""
        lines = [f"class_name S{i:04}", "extends Node2D", "", "@export pub var speed := 1.0", "",
                 "pub func value() -> int:", f"\treturn {i}", "",
                 f"pub func step(delta: float{prev}) -> float:", "\tvar total := speed * delta", "\t# note"]
        lines += ["\ttotal += prev.speed"] if i else []
        lines += ["\treturn total", ""]
        if i % 3 == 0 and i < 3 * SCENES:
            lines += ["func _ready() -> void:", "\t$Child.position = Vector2(1, 2)", ""]
        write(project / "scripts" / f"s{i:04}.pet", "\n".join(lines))
    for i in range(SCENES):
        resources = [f'[ext_resource type="Script" path="res://scripts/s{3 * i:04}.pet" id="1"]',
                     f'[ext_resource type="Script" path="res://scripts/s{3 * i + 1:04}.pet" id="2"]']
        nodes = ['[node name="Root" type="Node2D"]', 'script = ExtResource("1")', "",
                 '[node name="Child" type="Node2D" parent="."]', 'script = ExtResource("2")', ""]
        if i % DEPTH:
            resources.append(f'[ext_resource type="PackedScene" path="res://scenes/scene_{i - 1:03}.tscn" id="3"]')
            nodes += ['[node name="Inner" parent="." instance=ExtResource("3")]', ""]
        write(project / "scenes" / f"scene_{i:03}.tscn", "[gd_scene format=3]\n\n" + "\n".join(resources) + "\n\n" + "\n".join(nodes))
    for i in range(GD):
        write(project / "gd" / f"g{i:02}.gd", "extends Node\n\nvar speed := 1.0\n\nfunc go() -> void:\n\tpass\n")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    godot = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="petrichor-large-") as tmp:
        project = Path(tmp)
        generate(project)
        start = time.perf_counter()
        import_project(godot, project)
        imported = time.perf_counter() - start
        # The engine prints each load of a file (core/io/resource_loader.cpp:279; ed1daf0).
        output, built = run(godot, "-e", "--verbose", "--path", project, "--", "--petrichor-build")
    found = re.search(r"^petrichor build: (\d+) compile\(s\) outside the analysis, (\d+) check\(s\)$", output, re.M)
    if not found:
        raise RuntimeError(f"The build printed no counts\n{output}")
    counts = {"compiles outside the analysis": int(found[1]), "scene loads": len(re.findall(r"^Loading resource: \S+\.tscn ", output, re.M)), "checks": int(found[2])}
    print(f"import {imported:.1f} s, build {built / 1000:.1f} s (not held)")
    over = False
    for name, count in counts.items():
        line = f"{name}: {count}, budget {BUDGET[name]}"
        if count > BUDGET[name]:
            line += ": over"
            over = True
        elif count < BUDGET[name]:
            line += ": under, lower the budget"
        print(line)
    sys.exit(int(over))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(str(error))
