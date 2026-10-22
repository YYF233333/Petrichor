extends Node2D
func _ready():
	$A.text = "x"
	%U.show()
	get_node("B/C").hide()
	var q = 10 % 3
