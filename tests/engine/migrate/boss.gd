extends Enemy

signal roared


func roar() -> void:
	await roared
	speed *= 2.0
