extends RefCounted

signal tick
var total := 0

func once():
	await tick
	total += 1

func run(n: int) -> int:
	total = 0
	for i in n:
		once()
		tick.emit()
	return total
