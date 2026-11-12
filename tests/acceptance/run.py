"""The release's acceptance test: without a C++ toolchain, a project is
edited, exported and run on the host (architecture §5, step 10).

Usage:
    python tests/acceptance/run.py <godot editor binary> <export templates directory> <addon zip> [<demo zip>]

The editor and the export templates are the official ones of Godot 4.7. The
templates directory holds the host's templates, as the official .tpz has them
in templates/. The zips are those tools/package.py writes. Each engine run has
the limit PETRICHOR_TEST_TIMEOUT, in seconds, 60 by default.
"""

import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "engine"))
import run as engine  # noqa: E402

LINE = "acceptance: the squares of 1 to 10 add up to 385"

# The host's templates for a debug and a release export
# (platform/windows/export/export_plugin.cpp:374-376,
# platform/linuxbsd/export/export_plugin.cpp:157-159,
# platform/macos/export/export_plugin.cpp:1647; ed1daf0), the file an export
# writes, and the game's executable in it.
TEMPLATES, EXPORTED, EXECUTABLE = {
    "Windows Desktop": (("windows_debug_x86_64.exe", "windows_release_x86_64.exe"), "acceptance.exe", "acceptance.exe"),
    "Linux": (("linux_debug.x86_64", "linux_release.x86_64"), "acceptance.x86_64", "acceptance.x86_64"),
    "macOS": (("macos.zip", "macos.zip"), "acceptance.app", "acceptance.app/Contents/MacOS/acceptance"),
}[engine.HOST_PLATFORM]


def use_templates(project, templates):
    """Names the templates in the host's preset, which an export then takes in
    place of those installed in the editor's data directory
    (editor/export/editor_export_platform_pc.cpp:161-170,
    platform/macos/export/export_plugin.cpp:1638-1647; ed1daf0)."""
    debug, release = (Path(templates).resolve() / name for name in TEMPLATES)
    presets = project / "export_presets.cfg"
    text = presets.read_text(encoding="utf-8")
    index = re.search(rf'^\[preset\.(\d+)\]\n\nname="{engine.HOST_PLATFORM}"$', text, flags=re.M).group(1)
    options = f'custom_template/debug="{debug.as_posix()}"\ncustom_template/release="{release.as_posix()}"\n'
    presets.write_text(text.replace(f"[preset.{index}.options]\n\n", f"[preset.{index}.options]\n\n{options}"), encoding="utf-8")


def build(godot, name, project):
    """Imports the project, as an editor opened on it does, and builds it. The
    import is not checked, as import_project() of tests/engine/run.py explains."""
    engine.import_project(godot, project)
    code, output = engine.run(godot, "-e", "--path", str(project), "--", "--petrichor-build")
    engine.expect(f"{name}: edit", code == 0 and ", 0 with errors" in output, output)


def check_project(godot, templates, addon, tmp):
    project = tmp / "project"
    shutil.copytree(HERE / "project", project)
    zipfile.ZipFile(addon).extractall(project)
    use_templates(project, templates)
    build(godot, "project", project)
    code, output = engine.run(godot, "--path", str(project))
    engine.expect_run("project: run in the editor's binary", code, output, [LINE])
    for kind in ("debug", "release"):
        exported = tmp / kind / EXPORTED
        exported.parent.mkdir()
        code, output = engine.run(godot, "--path", str(project), f"--export-{kind}", engine.HOST_PLATFORM, str(exported))
        if engine.expect_run(f"project: {kind} export", code, output):
            code, output = engine.run(str(tmp / kind / EXECUTABLE))
            engine.expect_run(f"project: {kind} export runs", code, output, [LINE])


def check_demo(godot, demo, tmp):
    """The demo's main scene runs for 2 s of game time. It may report what the
    FPS starter kit's original reports in a headless run that ends while
    sounds play, which tests/engine/run.py's fps group allows too."""
    zipfile.ZipFile(demo).extractall(tmp / "demo")
    project = next((tmp / "demo").glob("*/project.godot")).parent
    build(godot, "demo", project)
    code, output = engine.run(godot, "--path", str(project), "--fixed-fps", "60", "--quit-after", "120")
    errors = [line for line in output.splitlines() if "ERROR" in line and not re.match(r"ERROR: \d+ resources still in use at exit", line)]
    engine.expect("demo: run", code == 0 and not errors, output)


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    godot, templates, addon = sys.argv[1:4]
    with tempfile.TemporaryDirectory() as tmp:
        check_project(godot, templates, addon, Path(tmp))
        if len(sys.argv) == 5:
            check_demo(godot, sys.argv[4], Path(tmp))
    print(f"{len(engine.failures)} failure(s)")
    sys.exit(1 if engine.failures else 0)


if __name__ == "__main__":
    main()
