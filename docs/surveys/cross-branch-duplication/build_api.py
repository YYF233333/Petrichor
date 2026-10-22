"""Extract per-class API member names (methods, properties, signals, constants) and the parent class from a Godot
checkout's class reference XML (doc/classes, modules/*/doc_classes, platform/*/doc_classes). Read-only on the checkout.

Usage: python build_api.py GODOT_DIR API_JSON   -> writes API_JSON (records the engine commit; the survey used 469659e)
"""
import glob
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

EXPECTED = "469659e0617345149cd1eaec2e643d61d130aa6a"
if len(sys.argv) != 3:
    sys.exit(__doc__)
root, out = sys.argv[1], sys.argv[2]
files = glob.glob(os.path.join(root, "doc", "classes", "*.xml"))
files += glob.glob(os.path.join(root, "modules", "*", "doc_classes", "*.xml"))
files += glob.glob(os.path.join(root, "platform", "*", "doc_classes", "*.xml"))
classes = {}
for f in files:
    try:
        t = ET.parse(f).getroot()
    except ET.ParseError:
        continue
    name = t.get("name")
    if not name:
        continue
    d = {"inherits": t.get("inherits"), "methods": [], "members": [], "signals": [], "constants": []}
    for tag, key in (("methods/method", "methods"), ("members/member", "members"), ("signals/signal", "signals"),
                     ("constants/constant", "constants")):
        for e in t.findall(tag):
            n = e.get("name")
            if n:
                d[key].append(n)
            if key == "constants" and e.get("enum"):
                d["constants"].append(e.get("enum"))
    for k in ("methods", "members", "signals", "constants"):
        d[k] = sorted(set(d[k]))
    classes[name] = d
commit = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
if commit != EXPECTED:
    print(f"warning: {root} is at {commit or 'unknown commit'}, the survey used {EXPECTED}", file=sys.stderr)
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with open(out, "w", encoding="utf-8", newline="\n") as fh:
    json.dump({"godot_commit": commit, "classes": classes}, fh, indent=0, sort_keys=True)
print(f"{len(classes)} classes from {commit}")
