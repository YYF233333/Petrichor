"""Writes the two packages of a release (architecture §5, step 10).

Usage:
    python tools/package.py <libraries> <version> <output directory> [--allow-missing]

<libraries> holds the libraries that tests/engine/project/petrichor.gdextension
lists, at any depth, as `gh run download` leaves them. Each one must be there,
unless --allow-missing is given.

petrichor-<version>-addon.zip unzips into a project as addons/petrichor/.
petrichor-<version>-demo.zip is the FPS starter kit with its migrated scripts
and the plugin, in the directory petrichor-<version>-demo/.
"""

import argparse
import io
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests" / "engine"))
import run as engine_tests  # noqa: E402

EXTENSION = engine_tests.PROJECT / "petrichor.gdextension"
ADDON = "addons/petrichor"
LIBRARY_SUFFIXES = (".dll", ".so", ".dylib")


def git(*args, cwd=ROOT):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, check=True).stdout


def pinned(submodule):
    return git("rev-parse", f"HEAD:{submodule}").decode().strip()


def addon_files(libraries, allow_missing):
    """The plugin's files by their paths in a project."""
    found = {path.name: path for path in Path(libraries).rglob("*") if path.is_file()}
    files = {f"{ADDON}/petrichor.gdextension": EXTENSION.read_bytes()}
    missing = []
    for _, library in engine_tests.libraries():
        if Path(library).name in found:
            files[f"{ADDON}/{library}"] = found[Path(library).name].read_bytes()
        else:
            missing.append(library)
    if missing:
        listed = "".join(f"\n    {library}" for library in missing)
        if not allow_missing:
            sys.exit(f"{libraries} lacks these libraries of {EXTENSION.name}:{listed}")
        print(f"Left out, as missing:{listed}")
    files[f"{ADDON}/LICENSE.md"] = (ROOT / "LICENSE").read_bytes()
    godot_cpp = (ROOT / "thirdparty" / "godot-cpp" / "LICENSE.md").read_text(encoding="utf-8")
    files[f"{ADDON}/THIRDPARTY.md"] = (
        "# Third-party notices\n\n"
        "Each library in bin/ links godot-cpp statically.\n"
        f"It is https://github.com/godotengine/godot-cpp at commit {pinned('thirdparty/godot-cpp')}, under this licence:\n\n"
        f"{godot_cpp}"
    ).encode()
    return files


def demo_files(addon, top):
    """The FPS starter kit at its pinned commit, with the migrated scripts in
    place of its own (architecture §5 step 4) and the plugin."""
    kit = pinned("thirdparty/starter-kit-fps")
    with tempfile.TemporaryDirectory() as tmp:
        project = Path(tmp)
        with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", kit, cwd=engine_tests.FPS_UPSTREAM))) as tar:
            tar.extractall(project, filter="data")
        engine_tests.migrate(project, engine_tests.FPS_MIGRATED)
        files = {f"{top}/{path.relative_to(project).as_posix()}": path.read_bytes() for path in project.rglob("*") if path.is_file()}
    files[f"{top}/NOTICE.md"] = (
        "# Notice\n\n"
        "The .pet scripts of this project are translated from the GDScript files of Kenney's Starter Kit FPS.\n"
        f"It is https://github.com/KenneyNL/Starter-Kit-FPS at commit {kit}, under the licence in LICENSE.md.\n"
        "addons/petrichor/ holds Petrichor, with its licences.\n"
    ).encode()
    files.update({f"{top}/{path}": data for path, data in addon.items()})
    return files


def write_zip(path, files):
    """Sorted entries, all dated at the last commit, with Unix permissions."""
    date = time.gmtime(int(git("log", "-1", "--format=%ct")))[:6]
    with zipfile.ZipFile(path, "w") as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date)
            info.create_system = 3
            info.external_attr = (0o100755 if name.endswith(LIBRARY_SUFFIXES) else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, files[name])
    print(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("libraries")
    parser.add_argument("version")
    parser.add_argument("output")
    parser.add_argument("--allow-missing", action="store_true")
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    addon = addon_files(args.libraries, args.allow_missing)
    write_zip(output / f"petrichor-{args.version}-addon.zip", addon)
    write_zip(output / f"petrichor-{args.version}-demo.zip", demo_files(addon, f"petrichor-{args.version}-demo"))


if __name__ == "__main__":
    main()
