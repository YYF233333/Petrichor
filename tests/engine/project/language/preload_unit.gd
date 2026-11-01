extends Node2D

# A GDScript class without a class_name, which gd_view_check.pet refers to
# through preload (spec §2.1, §11.1).

const TILE_SIZE := 16.0

var level := 0


func _init(p_level := 1) -> void:
	level = p_level


static func make(p_level: int) -> Node2D:
	var made := Node2D.new()
	made.name = "made %d" % p_level
	return made
