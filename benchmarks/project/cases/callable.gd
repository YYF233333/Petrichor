extends RefCounted

func step(value: int) -> int:
	return value + 1

func run(n: int) -> int:
	var callback: Callable = Callable(step)
	var total := 0
	for i in n:
		total += int(callback.call(i))
	return total
