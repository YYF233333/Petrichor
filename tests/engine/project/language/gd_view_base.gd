class_name GdViewBase
extends Node

# A GDScript class that gd_view_check.pet sees through reflection (spec §11.1).

signal scored(points: int)

const BONUS := 5

var speed: float = 1.5
var buddy: GdViewBase
var scores: Dictionary[String, int] = {}


func hit(amount: int) -> int:
	return amount * 2


func tally(by: Dictionary[String, int]) -> Dictionary[String, int]:
	return by


static func scaled(amount: int) -> int:
	return amount * BONUS


# Yields 1 and 2 to a for loop (spec §5.7).
func _iter_init(iter: Array) -> bool:
	iter[0] = 1
	return true


func _iter_next(iter: Array) -> bool:
	iter[0] += 1
	return iter[0] < 3


func _iter_get(iter: Variant) -> int:
	return iter
