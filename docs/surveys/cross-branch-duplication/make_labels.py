"""Manual mechanism labels for every exact (K=5) cross-branch clone group, joined with OUT_DIR/groups_exact.csv -> labels.csv.

Labels are judgement calls made by reading OUT_DIR/snippets_exact.txt:
  a = interface default methods only: methods that read members each class declares separately (or engine API on self)
      and declare/write no shared state
  b = stateful mixin: the copy declares fields/@export/signals, or writes fields whose declarations are also duplicated
  c = composition: a self-contained sub-feature (own state + nodes) that could be a child node/scene
  d = static helper: computation on parameters/globals/autoloads, no self state needed
  e = not really removable (trivial or scene-specific boilerplate)
A group is identified by its repo and its first copy (file:first-last(lines)), which does not depend on file order.
Usage: python make_labels.py OUT_DIR [--labels LABELS_CSV]   (default: labels.csv next to this script)
"""
import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
L = {
    # games
    ("Ring", "breakout/scenes/bricks/base/brick.gd:57-94(20)"): ("b", "check_init_type/set_type/show_health/_show_lucky_drop/hit_by_*: write type, read sprites/health_bar/health_component, all declared identically"),
    ("Ring", "breakout/scenes/bricks/base/brick.gd:44-50(6)"): ("b", "_ready body initialising health_bar/type"),
    ("Ring", "breakout/scenes/bricks/base/brick.gd:17-22(5)"): ("b", "5 identical @export fields (Bonus group)"),
    ("Ring", "breakout/scenes/bricks/base/brick.gd:108-114(5)"): ("b", "_on_health_change/_on_died over health_component field"),
    ("tanks-of-freedom-ii", "scenes/ui/menu/skirmish/skirmish.gd:79-87(8)"): ("d", "count sides in map_data; 5 copies in 2 services + 3 menus"),
    ("tanks-of-freedom-ii", "scenes/board/board.gd:992-997(6)"): ("a", "restart-match handler reading injected match_setup/switcher/audio"),
    ("tanks-of-freedom-ii", "scenes/ui/board/start_turn.gd:31-36(6)"): ("e", "_reset_labels: hide() on 5 same-named child refs"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/quest_log_panel.gd:47-63(15)"): ("c", "code-built quest detail column (3 menus)"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/quest_log_panel.gd:70-79(7)"): ("c", "code-built quest detail scroll body"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/mastery_panel.gd:285-299(11)"): ("d", "_loadout_power_used(picks, tree): pure"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/mastery_panel.gd:275-279(5)"): ("d", "mastery node display-name lookup: pure"),
    ("godot-tiny-mmo", "source/common/gameplay/leaderboard/champion_statue.gd:112-117(6)"): ("c", "create clickable Area child and wire hover signals (Area2D vs CharacterBody2D hosts)"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/equipment_panel.gd:65-73(6)"): ("a", "subscribe to local player's equipment_changed, refresh if visible"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/mastery_panel.gd:33-40(6)"): ("d", "layout container construction lines"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/quest_log_panel.gd:26-30(5)"): ("d", "layout container construction lines"),
    ("godot-tiny-mmo", "source/client/ui/menus/character/quest_log_panel.gd:152-156(5)"): ("d", "header Label factory"),
    ("godot-tiny-mmo", "source/client/ui/hud/trade_panel.gd:523-529(5)"): ("c", "countdown tween driving a label (HUD panel vs world trade table)"),
    ("godot-tiny-mmo", "source/client/ui/hud/trade_panel.gd:533-537(5)"): ("c", "countdown stop, same feature as the countdown start"),
    ("godot-tiny-mmo", "source/common/gameplay/combat/deflect_bubble.gd:34-41(5)"): ("b", "timed visual effect: _elapsed/duration fields, queue_free at end"),
    ("godot-tiny-mmo", "source/common/gameplay/maps/components/ambient_light/ambient_light.gd:7-11(5)"): ("b", "@export enabled whose setter toggles set_process"),
    ("deck_builder_tutorial", "scenes/enemy/enemy.gd:93-103(8)"): ("a", "take_damage reading stats/sprite_2d/modifier_handler (declared separately)"),
    ("deck_builder_tutorial", "scenes/status_handler/status_ui.gd:11-16(5)"): ("b", "set_status writes status field"),
    ("godot-open-rts", "source/generic-scenes-and-nodes/3d/RectangularSelection3D.gd:19-34(7)"): ("a", "_process/_input calling each class's own _start/_finish/_interrupt"),
    # demos
    ("godot-demo-projects", "2d/physics_tests/utils/characterbody_controller.gd:19-45(22)"): ("b", "per-frame input/velocity logic over identical fields (CharacterBody2D vs RigidBody2D)"),
    ("godot-demo-projects", "2d/physics_tests/utils/characterbody_controller.gd:3-8(6)"): ("b", "6 identical field declarations"),
    ("godot-demo-projects", "misc/hdr_output/max_color.gd:28-47(12)"): ("d", "linear colour limiting: pure function of colour, limit, flag, max"),
    ("godot-demo-projects", "misc/hdr_output/max_color.gd:6-24(7)"): ("b", "2 exports + window signal wiring in _ready/_exit_tree"),
    ("godot-demo-projects", "xr/openxr_hand_tracking_demo/hand_controller.gd:10-16(6)"): ("d", "XR pose selection fragment on locals"),
    # plugins/tools
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:25-49(22)"): ("b", "exports with setters (input_action_names, readable_action_names)"),
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:73-81(8)"): ("b", "member + _refresh_readable_action_names writing exported field"),
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:228-255(26)"): ("b", "_get_all_action_names over duplicated exported/member state"),
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:260-265(5)"): ("b", "method over duplicated state"),
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:287-298(11)"): ("b", "_build_assigned_input_events writes assigned_input_events"),
    ("Godot-Game-Template", "addons/maaacks_game_template/base/nodes/menus/options_menu/input/input_actions_list.gd:301-306(6)"): ("b", "writes last_input_readable_name, emits already_assigned"),
    ("godot_dialogue_manager", "addons/dialogue_manager/nodes/actionable/dialogue_actionable_2d.gd:13-55(21)"): ("b", "2 signals, exports with setters, members, _ready (Area2D vs Area3D)"),
    ("godot_dialogue_manager", "addons/dialogue_manager/nodes/actionable/dialogue_actionable_2d.gd:64-79(8)"): ("b", "nearest-actionable search + handler emitting duplicated signal"),
    ("beehave", "addons/beehave/debug/new_graph_edit.gd:139-146(8)"): ("d", "_get_icon(type): pure lookup in global class list"),
    ("Pixelorama", "src/Tools/BaseDraw.gd:318-322(5)"): ("d", "tilemap-layer undo/redo fragment inside 7 tool/dialog methods"),
    ("Pixelorama", "src/Autoload/DrawingAlgos.gd:66-83(17)"): ("d", "metadata image built in DrawingAlgos autoload and re-copied in a dialog"),
    ("Pixelorama", "src/UI/Timeline/LayerButton.gd:443-452(10)"): ("a", "_get_layer_indices reads layer_index (declared separately)"),
    ("Pixelorama", "src/Autoload/OpenSave.gd:764-771(6)"): ("d", "linked-cel undo fragment"),
    ("Pixelorama", "src/UI/Timeline/AnimationTimeline.gd:484-488(5)"): ("d", "selected frame indices"),
    ("Pixelorama", "src/UI/Canvas/CanvasPreview.gd:101-105(5)"): ("d", "play-only-tags frame range"),
    ("Pixelorama", "src/UI/Timeline/AnimationTagUI.gd:61-69(5)"): ("d", "animation tag undo/redo"),
    ("Pixelorama", "src/UI/Dialogs/ImportTagDialog.gd:228-235(7)"): ("d", "tag range shifting"),
    ("Pixelorama", "src/UI/Timeline/AnimationTimeline.gd:699-705(7)"): ("d", "tag range shrinking"),
    ("Pixelorama", "src/UI/Timeline/AnimationTimeline.gd:708-712(5)"): ("d", "tag range growing"),
    ("Pixelorama", "src/UI/Timeline/AnimationTimeline.gd:738-742(5)"): ("d", "undo/redo commit tail"),
    ("Pixelorama", "src/Autoload/Tools.gd:1017-1023(6)"): ("d", "pixel pick in cel"),
    ("Pixelorama", "src/Autoload/DrawingAlgos.gd:447-455(5)"): ("d", "RenderingServer viewport setup"),
    ("Pixelorama", "src/Autoload/DrawingAlgos.gd:909-914(5)"): ("d", "used-rect merge"),
    ("Pixelorama", "src/Tools/BaseDraw.gd:1006-1011(5)"): ("d", "tile rotation index arithmetic"),
    ("Pixelorama", "src/UI/Timeline/LayerEffects/LayerEffectFoldable.gd:58-62(5)"): ("a", "_get_region_rect from get_global_rect() on self"),
    ("phantom-camera", "addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:1046-1067(17)"): ("d", "_smooth_damp: pure maths on arguments"),
    ("phantom-camera", "addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:1168-1180(8)"): ("d", "_set_layer bitmask helper (also in PhantomCameraHost)"),
}
# Every other phantom-camera group (33 of them) is labelled b by one rule: each is part of the PhantomCamera2D/3D or
# NoiseEmitter2D/3D twins and refers to members whose declarations are duplicated in the twin as well.
PCAM_B = "PhantomCamera2D/3D or NoiseEmitter2D/3D: exported properties with setters/getters, private state, lifecycle"

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("out")
ap.add_argument("--labels", default=os.path.join(HERE, "labels.csv"))
args = ap.parse_args()
rows = list(csv.DictReader(open(os.path.join(args.out, "groups_exact.csv"), encoding="utf-8")))
out = []
missing = []
used = set()
for r in rows:
    first = r["members"].split(";")[0]
    key = (r["repo"], first)
    if key in L:
        lab, note = L[key]
        used.add(key)
    elif r["repo"] == "phantom-camera":
        lab, note = "b", PCAM_B
    else:
        missing.append(r["gid"])
        continue
    out.append({"gid": r["gid"], "group": r["group"], "repo": r["repo"], "size_all_copies": r["size_all_copies"], "label": lab,
                "native_pairs": r["native_pairs"], "forced": r["forced"], "heuristic": r["heuristic"], "note": note, "example": first})
with open(args.labels, "w", newline="", encoding="utf-8") as fh:
    w = csv.DictWriter(fh, fieldnames=list(out[0].keys()), lineterminator="\n")
    w.writeheader()
    w.writerows(out)
unused = sorted(set(L) - used)
print(f"{len(out)} labelled, groups without a label: {missing}, labels without a group: {unused}")
if missing or unused:
    sys.exit(1)
