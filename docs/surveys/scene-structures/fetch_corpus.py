"""Fetch the scene-structure corpus: shallow, blobless, sparse clones pinned to fixed commits.

Usage: python fetch_corpus.py CORPUS [name ...]
Clones into CORPUS/<name>. Re-running is idempotent (skips repos already at the pinned commit).
Python 3 stdlib only; needs git on PATH.
"""
import os
import subprocess
import sys

# name, url, commit, group
REPOS = [
    # Existing Petrichor corpus (same commits as the earlier await/property surveys).
    ("beehave", "https://github.com/bitbrain/beehave.git", "fe589153fa780a3f88989578ceb5f539afffb7c2", "plugin"),
    ("gdUnit4", "https://github.com/MikeSchulze/gdUnit4.git", "9bc1a450682c9c0d337e1b8977535f0a710385e1", "plugin"),
    ("godot_dialogue_manager", "https://github.com/nathanhoad/godot_dialogue_manager.git", "7a72dcf64cfec8673d398606f2991d4cf81f2d11", "plugin"),
    ("godot-demo-projects", "https://github.com/godotengine/godot-demo-projects.git", "b761b4cd5718a7118f2c95eb55f181d0a5f004f1", "demos"),
    ("Godot-Game-Template", "https://github.com/Maaack/Godot-Game-Template.git", "6849d6c352dafe8ed54b5fa4f4b9774adfca31c8", "plugin"),
    ("godot-open-rpg", "https://github.com/gdquest-demos/godot-open-rpg.git", "d59f2f71bc046ac53239a4e590861269e7100a36", "game"),
    ("godot-statecharts", "https://github.com/derkork/godot-statecharts.git", "76d226a3efac66a72aea825382b320c94808f409", "plugin"),
    ("phantom-camera", "https://github.com/ramokz/phantom-camera.git", "21d3762492e2dc51b448f55438b89b3c19349f96", "plugin"),
    ("Pixelorama", "https://github.com/Orama-Interactive/Pixelorama.git", "38a8b03eafc56a2815c42ff53ef3f61db7cf432b", "tool"),
    ("Starter-Kit-3D-Platformer", "https://github.com/KenneyNL/Starter-Kit-3D-Platformer.git", "3fa8a04b1c01ab23db43123d4ce814a34c3fc7f0", "game"),
    ("Starter-Kit-City-Builder", "https://github.com/KenneyNL/Starter-Kit-City-Builder.git", "4535092b740b378b700efd9df9e27a631815b84a", "game"),
    ("Starter-Kit-FPS", "https://github.com/KenneyNL/Starter-Kit-FPS.git", "185fd2326d74a5cf858cffc616f87cf9696f9cc0", "game"),
    ("Starter-Kit-Racing", "https://github.com/KenneyNL/Starter-Kit-Racing.git", "2f2e5f2646dda89cb21d4e8539bab60c6e955dc8", "game"),
    ("tps-demo", "https://github.com/godotengine/tps-demo.git", "a82f15448e9b015440d3bbdf5e10801b260c4e9f", "game"),
    # Added games (HEAD on 2026-10-23).
    ("godot-open-rts", "https://github.com/lampe-games/godot-open-rts.git", "a628ad3bb6a9a903587a181a7ee3300dbf735e81", "game"),
    ("tanks-of-freedom-ii", "https://github.com/P1X-in/tanks-of-freedom-ii.git", "dbe629ea05691599dde4fd65ab9a13b78a4ccfe1", "game"),
    ("deck_builder_tutorial", "https://github.com/guladam/deck_builder_tutorial.git", "91b408bc660e9b7389892dd4242a4d76e5bc9e77", "game"),
    ("godot-tiny-mmo", "https://github.com/SlayHorizon/godot-tiny-mmo.git", "f4ed04f20963c3285117e58b1149fa1117658d3c", "game"),
    ("godot_open_star_fighter", "https://github.com/teeeece/godot_open_star_fighter.git", "6e7be5c1371b91fb9dbd7f3c8c67f8cfc967580c", "game"),
    ("Ring", "https://github.com/YYF233333/Ring.git", "0ec41610f25d83917e6410451ad6d796a948c766", "game"),
]

PATTERNS = [
    "*.gd", "*.tscn", "*.tres", "*.scn", "*.uid", "*.cs",
    "project.godot", "plugin.cfg", "/LICENSE*", "/COPYING*", "/license*",
]


def git(cwd, *args):
    print("+ git", " ".join(args), flush=True)
    subprocess.run(["git", *args], cwd=cwd, check=True)


def head(cwd):
    r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=cwd, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def fetch(corpus, name, url, sha):
    dest = os.path.join(corpus, name)
    if os.path.isdir(os.path.join(dest, ".git")) and head(dest) == sha:
        print(f"{name}: already at {sha}")
        return
    os.makedirs(dest, exist_ok=True)
    git(dest, "init", "-q")
    subprocess.run(["git", "remote", "remove", "origin"], cwd=dest, capture_output=True)
    git(dest, "remote", "add", "origin", url)
    git(dest, "config", "core.longpaths", "true")
    git(dest, "sparse-checkout", "set", "--no-cone", *PATTERNS)
    git(dest, "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", sha)
    git(dest, "checkout", "-q", "--detach", sha)
    assert head(dest) == sha, name


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    corpus = sys.argv[1]
    want = set(sys.argv[2:])
    os.makedirs(corpus, exist_ok=True)
    for name, url, sha, _ in REPOS:
        if want and name not in want:
            continue
        fetch(corpus, name, url, sha)


if __name__ == "__main__":
    main()
