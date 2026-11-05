extends RefCounted

signal tick(value: int)
var total := 0

func receive(value: int):
	total += value

func run(n: int) -> int:
	total = 0
	tick.connect(receive)
	for i in n:
		tick.emit(i)
	tick.disconnect(receive)
	return total
