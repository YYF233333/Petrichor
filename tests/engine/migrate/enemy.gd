class_name Enemy
extends Node

@export var speed := 1.0
@export var stats: Stats
var health := 10


func hit(amount: int) -> void:
	health -= amount
