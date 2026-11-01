class_name GdViewBase
extends Node

# A GDScript class that gd_view_check.pet sees through reflection (spec §11.1).

signal scored(points: int)

const BONUS := 5

var speed: float = 1.5
var buddy: GdViewBase


func hit(amount: int) -> int:
	return amount * 2


static func scaled(amount: int) -> int:
	return amount * BONUS
