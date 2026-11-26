extends Brick

#var sprites : Array[Texture] = [
	#preload("res://assets/Brick-Blue.png"),
	#preload("res://assets/Brick-Green.png"),
	#preload("res://assets/Brick-Yellow.png"),
	#preload("res://assets/Brick-Orange.png"),
	#preload("res://assets/Brick-Red.png")
#]

func _individual_ready():
	point = type * 50 + 100
	drop_percent_scale = type * 0.125 + 1.0

func check_type():
	#debug
	if type != health_component.current_health - 1:
		print_debug("type seems not correct")


func physic_hurt(value: int):
	health_component.take_damage(min(value,1))


func _on_health_change():
	var current_health = health_component.current_health
	if current_health > 0:
		set_type(current_health-1)
		show_health()
