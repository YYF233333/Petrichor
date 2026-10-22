"""Self-test of the pipeline on a synthetic project (two scenes, inherited scene, editable children, subclass).

Usage: python test_fixture.py [--write]
Runs analyze.analyze_repo on fixture_corpus/fx next to this script and checks the expected results.
--write first regenerates fixture_corpus/fx from the definitions below.
"""
import os
import shutil
import sys

import analyze as A

FIXTURE = os.path.join(A.HERE, "fixture_corpus")
D = os.path.join(FIXTURE, "fx")

FILES = {
    "project.godot": "config_version=5\n",
    "shared.gd": 'extends Node2D\nfunc _ready():\n\t$A.text = "x"\n\t%U.show()\n\tget_node("B/C").hide()\n\tvar q = 10 % 3\n',
    "one.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://shared.gd" id="1"]\n'
                '[node name="R" type="Node2D"]\nscript = ExtResource("1")\n[node name="A" type="Label" parent="."]\n'
                '[node name="U" type="Sprite2D" parent="A"]\nunique_name_in_owner = true\n[node name="B" type="Node" parent="."]\n[node name="C" type="Timer" parent="B"]\n',
    "two.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://shared.gd" id="1"]\n'
                '[node name="R" type="Node2D"]\nscript = ExtResource("1")\n[node name="A" type="Sprite2D" parent="."]\n'
                '[node name="U" type="Sprite2D" parent="."]\nunique_name_in_owner = true\n',
    "inh.tscn": '[gd_scene format=3]\n[ext_resource type="PackedScene" path="res://one.tscn" id="1"]\n'
                '[node name="R2" instance=ExtResource("1")]\n[node name="Extra" type="Node" parent="B"]\n',
    "outer.tscn": '[gd_scene format=3]\n[ext_resource type="PackedScene" path="res://one.tscn" id="1"]\n'
                  '[node name="O" type="Node"]\n[node name="I" parent="." instance=ExtResource("1")]\n[node name="New" type="Node" parent="I/B"]\n[editable path="I"]\n'
                  '[node name="J" parent="." instance=ExtResource("1")]\n',
    "base.gd": "class_name Base\nextends Node2D\nfunc f():\n\t$K.hide()\n\tvar o = owner.name\n\tget_parent().get_node(\"Z\")\n",
    "s1.gd": "extends Base\n",
    "s2.gd": 'extends "res://base.gd"\n',
    "p.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://s1.gd" id="1"]\n[node name="P" type="Node2D"]\nscript = ExtResource("1")\n[node name="K" type="Label" parent="."]\n',
    "q.tscn": '[gd_scene format=3]\n[ext_resource type="Script" path="res://s2.gd" id="1"]\n[node name="Q" type="Node2D"]\nscript = ExtResource("1")\n[node name="K" type="Button" parent="."]\n',
}

if "--write" in sys.argv[1:]:
    shutil.rmtree(FIXTURE, ignore_errors=True)
    for rel, text in FILES.items():
        p = os.path.join(D, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

rs, rows, uses, ctx, dbg = A.analyze_repo("fx", "game", FIXTURE)
base = [r for r in rows if r["script"] == "base.gd"][0]
print({k: base[k] for k in ("bucket", "zero_reason", "n_ctx_all", "n_struct_all", "mech", "st", "owner_dot", "other_recv_lit", "get_parent")})
assert base["bucket"] == "0" and "M5_subclass_diff" in base["mech"] and base["st"] == {"differs_native": 1}
assert base["owner_dot"] == 1 and base["other_recv_lit"] == 1 and base["get_parent"] == 1
rows = [r for r in rows if r["script"] == "shared.gd"]
uses = [u for u in uses if u["script"] == "shared.gd"]
for r in rows:
    print({k: r[k] for k in ("script", "bucket", "n_ctx_own", "n_struct_own", "mech", "lit_uses", "st", "plain_instances")})
for u in uses:
    print(u["path"], u["status"], u["types"], u["outcomes"])
print({k: rs[k] for k in ("inherited_scenes", "inherited_scenes_with_structural_change", "scenes_adding_into_instances", "editable_entries")})
st = {u["path"]: u["status"] for u in uses}
assert st["A"] == "differs_native", st
assert st["%U"] == "consistent", st
assert st["B/C"] == "missing_in_some", st
assert rows[0]["n_scenes"] == 2 and rows[0]["bucket"] == "2"
assert "M2_inherited_diff" in rows[0]["mech"] and "M3_instance_modified_diff" in rows[0]["mech"]
assert rows[0]["plain_instances"] == 1
print("fixture OK")
