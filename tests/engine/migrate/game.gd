extends Node

const Boss = preload("res://boss.gd")


func damage(enemy: Enemy) -> bool:
	enemy.hit(1)
	var parent: Node = enemy.get_parent()
	print(parent)
	return enemy is Boss
