# annotation_check.pet as GDScript writes it.
@static_unload
@icon("icons/annotation.svg")
class_name AnnotationReference
extends Node

@export_file("*.json", "*.txt") var data_file: String
@export_file_path var any_file: String
@export_dir var folder: String
@export_global_file("*.png") var image_file: String
@export_global_dir var global_folder: String
@export_placeholder("Name") var title: String
@export_multiline("Hint") var notes: String
@export_exp_easing("attenuation") var easing: float
@export_color_no_alpha var tint: Color
@export_node_path("Node3D", "Label") var target: NodePath
@export_flags("Fire", "Water:4", "Earth") var elements: int
@export_flags_2d_render var render_layers: int
@export_flags_3d_physics var physics_layers: int
@export_flags_avoidance var avoidance_layers: int
@export_file("*.png") var images: Array[String]
@export_range(0, 1, 0.1) var weights: PackedFloat32Array
@export_enum("Low", "High") var levels: Array[int]
@export_storage var saved := 3
@export_custom(PROPERTY_HINT_LINK, "suffix:m/s") var speed := Vector2.ONE
@export_custom(PROPERTY_HINT_NONE, "suffix:s", PROPERTY_USAGE_EDITOR) var delay := 0.5


@rpc("any_peer", "call_local", "unreliable_ordered", 2)
func hit(_amount: int):
	pass


@rpc
func sync_state():
	pass


@rpc("call_remote", "reliable")
func confirm():
	pass
