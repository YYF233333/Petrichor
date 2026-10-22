"""Extract the native class hierarchy (name -> parent) from a Godot checkout's class reference XML.

Usage: python build_classes.py GODOT_DIR
Writes classes.json next to this script and records the engine commit. The committed classes.json
was built from Godot commit 469659e0617345149cd1eaec2e643d61d130aa6a. Reads XML only; nothing is built.
"""
import glob
import json
import os
import re
import subprocess
import sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
root = sys.argv[1]
files = glob.glob(os.path.join(root, "doc", "classes", "*.xml"))
files += glob.glob(os.path.join(root, "modules", "*", "doc_classes", "*.xml"))
files += glob.glob(os.path.join(root, "platform", "*", "doc_classes", "*.xml"))
pat = re.compile(r'<class name="([^"]+)"(?:[^>]*?inherits="([^"]+)")?')
parents = {}
for f in files:
    with open(f, encoding="utf-8") as fh:
        head = fh.read(2000)
    m = pat.search(head)
    if m:
        parents[m.group(1)] = m.group(2)
commit = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "classes.json")
with open(out, "w", encoding="utf-8", newline="\n") as fh:
    json.dump({"godot_commit": commit, "parents": parents}, fh, indent=0, sort_keys=True)
print(f"{len(parents)} classes from {commit}")
