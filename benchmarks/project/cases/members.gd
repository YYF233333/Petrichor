extends RefCounted

var count := 0

func run(n: int) -> int:
	count = 0
	for i in n:
		count += i % 2
	return count
